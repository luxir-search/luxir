// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/StorageTestUtils.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "luxir/index/IndexWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanWeight.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/FuzzyQuery.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/TermRangeQuery.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/Stats.h"
#include "luxir/store/Directory.h"
#include "luxir/util/DateTime.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;

namespace {

FilterCacheConfig testConfig() {
  return {
      .maxBytes = 1024 * 1024,
      .maxEntryBytes = 512 * 1024,
      .minSegmentDocs = 0,
      .admissionHistorySize = 32,
      .admissionThreshold = 2,
      .maxMetadataEntries = 64,
      .maxMetadataBytes = 256 * 1024};
}

class ExactFilterCachePolicyGuard {
  bool saved;

public:
  explicit ExactFilterCachePolicyGuard(bool disabled)
    : saved(BooleanQuery::disableExactFilterCachePolicyForTests) {
    BooleanQuery::disableExactFilterCachePolicyForTests = disabled;
  }

  ~ExactFilterCachePolicyGuard() {
    BooleanQuery::disableExactFilterCachePolicyForTests = saved;
  }
};

class FilterOwnGuard {
  bool saved;

public:
  explicit FilterOwnGuard(bool disabled)
    : saved(QueryPrep::disableFilterOwnForTests) {
    QueryPrep::disableFilterOwnForTests = disabled;
  }

  ~FilterOwnGuard() {
    QueryPrep::disableFilterOwnForTests = saved;
  }
};

class SparseBatchPostingsFeedGuard {
  bool saved;

public:
  explicit SparseBatchPostingsFeedGuard(bool disabled)
    : saved(QueryPrep::disableSparseBatchPostingsFeedForTests) {
    QueryPrep::disableSparseBatchPostingsFeedForTests = disabled;
  }

  ~SparseBatchPostingsFeedGuard() {
    QueryPrep::disableSparseBatchPostingsFeedForTests = saved;
  }
};

class FilteredConjunctionPostingsFeedGuard {
  bool saved;

public:
  explicit FilteredConjunctionPostingsFeedGuard(bool disabled)
    : saved(BooleanQuery::
                disableFilteredConjunctionPostingsFeedForTests) {
    BooleanQuery::disableFilteredConjunctionPostingsFeedForTests =
        disabled;
  }

  ~FilteredConjunctionPostingsFeedGuard() {
    BooleanQuery::disableFilteredConjunctionPostingsFeedForTests =
        saved;
  }
};

class FilteredConjMultiTermGuard {
  bool saved;

public:
  explicit FilteredConjMultiTermGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredConjMultiTermForTests) {
    BooleanQuery::disableFilteredConjMultiTermForTests = disabled;
  }

  ~FilteredConjMultiTermGuard() {
    BooleanQuery::disableFilteredConjMultiTermForTests = saved;
  }
};

class CandidateTermFeedGuard {
  bool saved;

public:
  explicit CandidateTermFeedGuard(bool disabled)
    : saved(BooleanQuery::disableCandidateTermFeedForTests) {
    BooleanQuery::disableCandidateTermFeedForTests = disabled;
  }

  ~CandidateTermFeedGuard() {
    BooleanQuery::disableCandidateTermFeedForTests = saved;
  }
};

class CandidateTermFeedFractionGuard {
  int64_t saved;

public:
  explicit CandidateTermFeedFractionGuard(int64_t fraction)
    : saved(BooleanQuery::kTermFeedMaxLeadFractionForTests) {
    BooleanQuery::kTermFeedMaxLeadFractionForTests = fraction;
  }

  ~CandidateTermFeedFractionGuard() {
    BooleanQuery::kTermFeedMaxLeadFractionForTests = saved;
  }
};

class CandidateTermFeedDocSetRatioGuard {
  int64_t saved;

public:
  explicit CandidateTermFeedDocSetRatioGuard(int64_t ratio)
    : saved(BooleanQuery::kTermFeedMinDocSetFilterRatio) {
    BooleanQuery::kTermFeedMinDocSetFilterRatio = ratio;
  }

  ~CandidateTermFeedDocSetRatioGuard() {
    BooleanQuery::kTermFeedMinDocSetFilterRatio = saved;
  }
};

class FilteredConjunctionBatchGuard {
  bool saved;

public:
  explicit FilteredConjunctionBatchGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredConjunctionBatchForTests) {
    BooleanQuery::disableFilteredConjunctionBatchForTests = disabled;
  }

  ~FilteredConjunctionBatchGuard() {
    BooleanQuery::disableFilteredConjunctionBatchForTests = saved;
  }
};

class FilteredConjunctionBatchSizeGuard {
  int32_t saved;

public:
  explicit FilteredConjunctionBatchSizeGuard(int32_t size)
    : saved(BooleanQuery::ConjunctionBulkScorer::
                filteredConjunctionBatchSizeForTests) {
    BooleanQuery::ConjunctionBulkScorer::
        filteredConjunctionBatchSizeForTests = size;
  }

  ~FilteredConjunctionBatchSizeGuard() {
    BooleanQuery::ConjunctionBulkScorer::
        filteredConjunctionBatchSizeForTests = saved;
  }
};

class FilteredConjRatioGuard {
  int64_t savedDocSet;
  int64_t savedPostings;

public:
  explicit FilteredConjRatioGuard(int64_t ratio)
    : savedDocSet(BooleanQuery::multiTermBatchMinRatioDocSet),
      savedPostings(BooleanQuery::multiTermBatchMinRatioPostings) {
    BooleanQuery::multiTermBatchMinRatioDocSet = ratio;
    BooleanQuery::multiTermBatchMinRatioPostings = ratio;
  }

  ~FilteredConjRatioGuard() {
    BooleanQuery::multiTermBatchMinRatioDocSet = savedDocSet;
    BooleanQuery::multiTermBatchMinRatioPostings = savedPostings;
  }
};

class FilteredDisjunctionArrayFeedGuard {
  bool saved;

public:
  explicit FilteredDisjunctionArrayFeedGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredDisjunctionArrayFeedForTests) {
    BooleanQuery::disableFilteredDisjunctionArrayFeedForTests = disabled;
  }

  ~FilteredDisjunctionArrayFeedGuard() {
    BooleanQuery::disableFilteredDisjunctionArrayFeedForTests = saved;
  }
};

class FilteredDisjunctionPostingsBlockGatherGuard {
  bool saved;

public:
  explicit FilteredDisjunctionPostingsBlockGatherGuard(bool disabled)
    : saved(BooleanQuery::
                disableFilteredDisjunctionPostingsBlockGatherForTests) {
    BooleanQuery::disableFilteredDisjunctionPostingsBlockGatherForTests =
        disabled;
  }

  ~FilteredDisjunctionPostingsBlockGatherGuard() {
    BooleanQuery::disableFilteredDisjunctionPostingsBlockGatherForTests =
        saved;
  }
};

class OwnedFilterStatsGuard {
  bool saved;

public:
  OwnedFilterStatsGuard() : saved(SkipStats::enabled) {
    SkipStats::enabled = true;
    SkipStats::reset();
  }

  ~OwnedFilterStatsGuard() {
    SkipStats::enabled = saved;
    SkipStats::reset();
  }
};

class UncacheableQuery final : public Query {
  Query& child;

public:
  explicit UncacheableQuery(Query& child)
    : Query(QueryKind::TEST), child(child) {}

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    child.validateLogical(context, multiplier);
  }

  FilterKeyScope appendFilterKey(
      FilterKeyBuilder& out, const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    return FilterKeyScope::UNCACHEABLE;
  }

  Query::Weight* createWeight(
      Query::Context& context, int32_t flags,
      float multiplier = 1.0f) override {
    return child.createWeight(context, flags, multiplier);
  }
};

class FailSecondSupplierQuery final : public Query {
  Query& child;
  int32_t supplierCalls = 0;

  class Weight final : public Query::Weight {
    Query::Weight* child;
    int32_t* supplierCalls;

  public:
    Weight(Query::Context& context, const Query& query, int32_t flags,
           Query::Weight* child,
           int32_t* supplierCalls)
      : Query::Weight(context, query, flags), child(child),
        supplierCalls(supplierCalls) {
      traits = child->getFlags();
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      (*supplierCalls)++;
      if (*supplierCalls == 2) {
        throw std::runtime_error("injected raw materialization failure");
      }
      return child->scorerSupplier(targetPool, segment, executionMode);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return child->createScorer(targetPool, segment);
    }
  };

public:
  explicit FailSecondSupplierQuery(Query& child)
    : Query(QueryKind::TEST), child(child) {}

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    child.validateLogical(context, multiplier);
  }

  FilterKeyScope appendFilterKey(
      FilterKeyBuilder& out, const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    return child.appendFilterKey(out, ctx);
  }

  Query::Weight* createWeight(
      Query::Context& context, int32_t flags,
      float multiplier = 1.0f) override {
    auto* childWeight = child.createWeight(context, flags, multiplier);
    return context.pool.make<Weight>(
        context, *this, flags, childWeight, &supplierCalls);
  }

  int32_t calls() const { return supplierCalls; }
};

std::unique_ptr<DocSet> docs(int32_t maxDoc,
                             std::initializer_list<int32_t> values) {
  DocSetBuilder builder(maxDoc);
  for (int32_t value : values) builder.add(value);
  return builder.build();
}

std::unique_ptr<DocSet> bitDocs(int32_t maxDoc, int32_t value) {
  auto result = std::make_unique<RAMBitDocSet>(maxDoc);
  result->mutableBits().set(value);
  return result;
}

FilterKey keyFor(const Query& query, uint64_t schemaGen) {
  FilterKeyContext context;
  context.schemaGen = schemaGen;
  FilterKeyBuilder builder;
  FilterKeyScope scope = query.appendFilterKey(builder, context);
  return std::move(builder).finish(scope, context).value();
}

void addTermDoc(IndexWriter& writer, std::string_view text) {
  auto& inverter = writer.obtainInverter();
  auto& field = inverter.getIndexHandler("text_w");
  inverter.startDoc();
  field.index(inverter, text);
  inverter.finishDoc();
  writer.releaseInverter(inverter);
}

void addIdTermDoc(IndexWriter& writer, std::string_view idValue,
                  std::string_view text) {
  auto& inverter = writer.obtainInverter();
  auto& id = inverter.getIndexHandler("id");
  auto& field = inverter.getIndexHandler("text_w");
  inverter.startDoc();
  id.index(inverter, idValue);
  field.index(inverter, text);
  inverter.finishDoc();
  writer.releaseInverter(inverter);
}

struct CachedSearchResult {
  int64_t count;
  std::vector<std::string> ids;

  friend bool operator==(const CachedSearchResult&,
                         const CachedSearchResult&) = default;
};

CachedSearchResult runCachedSearch(LuxirNode& node, std::string_view collection,
                                   std::string_view filter) {
  auto request = luxir::test::localReq(node.getSearchEngine());
  request->collection(collection)
      .topDocs("q")
      .matchQuery("body_w", "body")
      .matchFilter("filter_w", filter)
      .fields({"id"})
      .countLimit()
      .limit(-1);
  request->execute();
  EXPECT_TRUE(request->ok()) << request->toString();
  CachedSearchResult result{
      .count = request->getMatchCount("q"), .ids = {}};
  for (const auto& doc : request->getDocs("q")) {
    auto* id = luxir::test::find(doc, "id");
    if (id != nullptr) result.ids.push_back(std::get<std::string>(*id));
  }
  std::sort(result.ids.begin(), result.ids.end());
  return result;
}

void installVectorSchema(Collection& collection) {
  SchemaBuilder schema;
  auto& vector = schema.templ("_v");
  vector.type = api::FieldDef_::FieldClass::VECTOR;
  vector.column = true;
  vector.metric = api::VectorMetric::L2;
  schema.set(collection);
}

void addFilter(luxir::test::OpCursor& cursor, const api::Query& query);

CachedSearchResult runKnnFilter(
    LuxirNode& node, std::string_view collection,
    std::span<const float> queryVector, int32_t k, bool exact = true,
    int32_t nprobe = 0, int32_t refineCandidates = 0,
    float minScanFraction = 0.0f) {
  auto request = luxir::test::localReq(node.getSearchEngine());
  auto& cursor = request->collection(collection)
                     .topDocs("q")
                     .allQuery()
                     .fields({"id"})
                     .countLimit()
                     .limit(-1);
  addFilter(cursor, luxir::test::qb::knn(
      cursor.mr(), "embedding_v", queryVector, k, nprobe, exact,
      refineCandidates, minScanFraction));
  request->execute();
  EXPECT_TRUE(request->ok()) << request->toString();
  CachedSearchResult result{
      .count = request->getMatchCount("q"), .ids = {}};
  for (const auto& doc : request->getDocs("q")) {
    auto* id = luxir::test::find(doc, "id");
    if (id != nullptr) result.ids.push_back(std::get<std::string>(*id));
  }
  std::sort(result.ids.begin(), result.ids.end());
  return result;
}

FilterKey knnKey(Collection& collection, IndexReader& reader,
                 std::span<const float> queryVector, int32_t k,
                 bool exact = true, int32_t nprobe = 0,
                 int32_t refineCandidates = 0,
                 float minScanFraction = 0.0f) {
  auto schema = collection.getSchema();
  auto* fieldType = dynamic_cast<VectorFieldType*>(
      schema->getFieldTypePtr("embedding_v"));
  if (fieldType == nullptr) {
    throw std::logic_error("test vector field is absent");
  }
  KnnQuery query("embedding_v", *fieldType, queryVector, k, nprobe,
                 refineCandidates, minScanFraction, exact);
  FilterKeyContext context{.schemaGen = schema->gen_,
                           .coreGen = reader.coreGen(),
                           .timeZone = {}};
  FilterKeyBuilder builder;
  FilterKeyScope scope = query.appendFilterKey(builder, context);
  return std::move(builder).finish(scope, context).value();
}

std::vector<DocSet*> canonicalDomains(IndexReader& reader) {
  std::vector<DocSet*> result;
  result.reserve(reader.segments().size());
  for (auto& segment : reader.segments()) {
    result.push_back(segment.liveDocSet());
  }
  return result;
}

std::vector<FilterCache::SegmentIdentity> readerIdentitiesForTest(
    IndexReader& reader) {
  std::vector<FilterCache::SegmentIdentity> result;
  result.reserve(reader.segments().size());
  for (auto& segment : reader.segments()) {
    result.push_back({segment.segInfo.seg_id, segment.maxDoc()});
  }
  return result;
}

std::vector<std::unique_ptr<DocSet>> oneDocPerSegment(IndexReader& reader) {
  std::vector<std::unique_ptr<DocSet>> result;
  result.reserve(reader.segments().size());
  for (auto& segment : reader.segments()) {
    DocSetBuilder builder(segment.maxDoc());
    if (segment.maxDoc() != 0) builder.add(0);
    result.push_back(builder.build());
  }
  return result;
}

void addFilter(luxir::test::OpCursor& cursor, const api::Query& query) {
  auto& top = std::get<api::TopDocs>(cursor.rawOp().kind);
  auto* filter = api::build::allocArray(top.filter, 1, cursor.mr());
  auto* stored = (api::Query*)cursor.mr().allocate(sizeof(api::Query),
                                                   alignof(api::Query));
  new (stored) api::Query(query);
  filter[0].query = stored;
}

struct FilterFoldGuard {
  bool previous;

  explicit FilterFoldGuard(bool disabled)
    : previous(disableTopDocsFilterFold) {
    disableTopDocsFilterFold = disabled;
  }
  ~FilterFoldGuard() {
    disableTopDocsFilterFold = previous;
  }
};

FilterCache::Probe::Kind servePolicyValue(
    FilterCache& cache,
    std::span<const FilterCache::SegmentIdentity> segments,
    const FilterKey& key, int32_t expectedDoc, uint64_t buildCostNanos,
    int* claimedBuilds = nullptr, bool* sharedHit = nullptr,
    uint64_t readerCoreGen = 1) {
  FilterCache::UseRegistry request(cache, readerCoreGen, segments);
  auto* use = request.get(key);
  auto probe = use->probe(0);
  auto kind = probe.kind();
  DocSet* served = nullptr;
  std::unique_ptr<DocSet> fallback;
  if (kind == FilterCache::Probe::Kind::HIT) {
    served = probe.docSet();
    if (sharedHit != nullptr) *sharedHit = true;
  } else if (kind == FilterCache::Probe::Kind::BUILD) {
    if (claimedBuilds != nullptr) (*claimedBuilds)++;
    auto value = use->publishRaw(
        0, probe, bitDocs(4096, expectedDoc), buildCostNanos);
    served = value->docSet();
  } else {
    fallback = bitDocs(4096, expectedDoc);
    served = fallback.get();
  }
  EXPECT_EQ(1, served->card());
  EXPECT_TRUE(served->get(expectedDoc));
  EXPECT_FALSE(served->get((expectedDoc + 1) % 4096));
  return kind;
}

} // namespace

TEST(FilterCacheTest, admissionIsOncePerDistinctKeyPerRequest) {
  FilterCache cache(testConfig());
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("tenant:a");

  FilterCache::UseRegistry first(cache, 1, segments);
  auto* firstUse = first.get(key);
  EXPECT_EQ(firstUse, first.get(key));
  EXPECT_EQ(1u, first.size());
  EXPECT_FALSE(firstUse->wasAdmitted());
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, firstUse->probe(0).kind());

  FilterCache::UseRegistry second(cache, 1, segments);
  auto* secondUse = second.get(key);
  EXPECT_TRUE(secondUse->wasAdmitted());
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, secondUse->probe(0).kind());
  EXPECT_EQ(1u, cache.counters().admissions);
  EXPECT_EQ(0u, cache.counters().builds)
      << "a claim is not a completed cache build";
}

// The two flush postures are distinct measurement scenarios: clear() alone
// keeps admission-lane sightings, so a known key rebuilds immediately.
// clear() + resetAdmission() restarts the observation ladder (first sighting). dump() reports resident entries
// with a printable key extraction, and resetCounters() zeroes cumulative
// stats without touching residency.
TEST(FilterCacheTest, flushPosturesAndDump) {
  FilterCache cache(testConfig());
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("flushposture:term");

  auto probeOnce = [&](bool publishOnBuild) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto probe = use->probe(0);
    auto kind = probe.kind();
    if (kind == FilterCache::Probe::Kind::BUILD && publishOnBuild) {
      use->publishRaw(0, probe, bitDocs(4096, 7), 10);
    }
    return kind;
  };

  // Ladder to residency: bypass, build, hit.
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, probeOnce(true));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, probeOnce(true));
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, probeOnce(true));

  size_t resident = 0;
  auto rows = cache.dump(10, &resident);
  ASSERT_EQ(1u, resident);
  ASSERT_EQ(1u, rows.size());
  EXPECT_NE(rows[0].keyText.find("flushposture:term"), std::string::npos);
  EXPECT_EQ(1u, rows[0].segmentsResident);
  EXPECT_GT(rows[0].bytes, 0u);
  EXPECT_EQ(10u, rows[0].buildNanos);
  EXPECT_EQ(10u, cache.counters().buildNanos);
  EXPECT_EQ(10u, cache.counters().estimatedSavedNanos);

  // Hot hits keep recording sightings while cold keys roll the ring past the
  // key's first two.
  for (int i = 0; i < 40; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::HIT, probeOnce(true));
    FilterCache::UseRegistry cold(cache, 1, segments);
    EXPECT_FALSE(cold.get(FilterKey("flushposture:cold" + std::to_string(i)))
                     ->wasAdmitted());
  }

  EXPECT_EQ(410u, cache.counters().estimatedSavedNanos);

  // Refill posture: values gone, sightings kept, so the next probe claims a
  // build immediately rather than restarting the ladder.
  cache.clear();
  EXPECT_EQ(0u, cache.dump(10, &resident).size());
  EXPECT_EQ(0u, resident);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, probeOnce(true));
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, probeOnce(true));

  // First-sighting posture: forgetting admission history restarts the ladder.
  cache.clear();
  cache.resetAdmission();
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, probeOnce(true));

  EXPECT_GT(cache.counters().hits, 0u);
  EXPECT_GT(cache.counters().builds, 0u);
  cache.resetCounters();
  auto counters = cache.counters();
  EXPECT_EQ(0u, counters.hits);
  EXPECT_EQ(0u, counters.misses);
  EXPECT_EQ(0u, counters.builds);
  EXPECT_EQ(0u, counters.admissions);
  EXPECT_EQ(0u, counters.buildNanos);
  EXPECT_EQ(0u, counters.estimatedSavedNanos);
  EXPECT_EQ(0u, counters.unpaidBuildNanos);
}

// resize() rederives the budgets the config left at zero and keeps explicit
// ones, sweeps a shrink down to the new budget, disables and empties
// the cache at zero, and re-enables it from there. The explicit metadata
// budget keeps the metadata sweep out of the shrink.
TEST(FilterCacheTest, resizeAtRuntime) {
  FilterCache cache({.maxBytes = 1024 * 1024, .minSegmentDocs = 0,
                     .admissionThreshold = 1,
                     .maxMetadataBytes = 64 * 1024});
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  auto key = [](int i) { return FilterKey("resize:" + std::to_string(i)); };
  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
              servePolicyValue(cache, segments, key(i), i, 10));
  }
  size_t full = cache.bytesUsed();
  ASSERT_GT(full, 0u);

  size_t shrunk = full / 2;
  cache.resize(shrunk);
  auto config = cache.configuration();
  EXPECT_EQ(shrunk, config.maxBytes);
  EXPECT_EQ(shrunk / 5, config.maxEntryBytes);
  EXPECT_EQ(64u * 1024, config.maxMetadataBytes);
  EXPECT_GT(cache.bytesUsed(), 0u);
  EXPECT_LE(cache.bytesUsed(), config.maxBytes);
  EXPECT_GT(cache.counters().evictions, 0u);
  cache.validateForTest();

  cache.resize(0);
  EXPECT_FALSE(cache.enabled());
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_EQ(0u, cache.entryCountForTest());
  EXPECT_EQ(0u, cache.configuration().maxEntryBytes);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
            servePolicyValue(cache, segments, key(0), 0, 10));

  cache.resize(1024 * 1024);
  EXPECT_TRUE(cache.enabled());
  EXPECT_EQ(1024u * 1024 / 5, cache.configuration().maxEntryBytes);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, key(0), 0, 10));
  EXPECT_EQ(FilterCache::Probe::Kind::HIT,
            servePolicyValue(cache, segments, key(0), 0, 10));
  cache.validateForTest();
}

TEST(FilterCacheTest, resizeGrowAndRepeatPreserveResidents) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  size_t budget = charge * 10 + 10;
  FilterCache cache({.maxBytes = budget, .minSegmentDocs = 0,
                     .admissionThreshold = 1, .maxMetadataBytes = 64 * 1024});
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  for (int i = 0; i < 10; i++) {
    servePolicyValue(cache, segments, FilterKey("grow:" + std::to_string(i)), i, 10);
  }
  size_t resident = cache.bytesUsed();
  ASSERT_LE(resident, cache.configuration().maxBytes);
  auto evictions = cache.counters().evictions;
  cache.resize(budget);
  cache.resize(budget + charge / 10);
  EXPECT_EQ(resident, cache.bytesUsed());
  EXPECT_EQ(evictions, cache.counters().evictions);
  cache.validateForTest();
}

TEST(FilterCacheTest, resizeDerivesLimitsAndRestoresExplicitWatermarks) {
  FilterCache derived({.maxBytes = 0});
  derived.resize(1000);
  auto config = derived.configuration();
  EXPECT_EQ(200u, config.maxEntryBytes);
  EXPECT_EQ(250u, config.maxMetadataBytes);
  EXPECT_EQ(225u, config.lowWatermarkMetadataBytes);
  derived.resize(100);
  EXPECT_EQ(25u, derived.configuration().maxMetadataBytes);
  EXPECT_EQ(22u, derived.configuration().lowWatermarkMetadataBytes);

  FilterCache explicitLimits({.maxBytes = 1000, .maxEntryBytes = 77, .maxMetadataBytes = 300,
      .lowWatermarkMetadataBytes = 400});
  explicitLimits.resize(10);
  explicitLimits.resize(2000);
  config = explicitLimits.configuration();
  EXPECT_EQ(77u, config.maxEntryBytes);
  EXPECT_EQ(300u, config.maxMetadataBytes);
  EXPECT_EQ(300u, config.lowWatermarkMetadataBytes);
  explicitLimits.resize(0);
  explicitLimits.resize(0);
  EXPECT_FALSE(explicitLimits.enabled());
  EXPECT_EQ(0u, explicitLimits.entryCountForTest());
}

TEST(FilterCacheTest, resizeWatermarksDoNotOverflow) {
  constexpr size_t maximum = std::numeric_limits<size_t>::max();
  FilterCache cache({.maxBytes = 0, .maxMetadataBytes = maximum});
  for (size_t bytes : std::array<size_t, 6>{0, 1, 9, 10, maximum / 9 + 1, maximum}) {
    cache.resize(bytes);
    auto config = cache.configuration();
    EXPECT_EQ(maximum - maximum / 10 - (size_t)(maximum % 10 != 0),
              config.lowWatermarkMetadataBytes);
  }
}

TEST(FilterCacheTest, resizeRejectedChargeLeavesNoRawValue) {
  for (size_t bytes : {0u, 1024u}) {
    FilterCache cache({.maxBytes = 1024 * 1024, .minSegmentDocs = 0,
                       .admissionThreshold = 1, .maxMetadataBytes = 64 * 1024});
    std::array segments{FilterCache::SegmentIdentity{1, 4096}};
    cache.onReaderPublished(1, segments);
    RequestMemTracker tracker(1);
    {
      FilterCache::UseRegistry request(cache, 1, segments);
      auto* use = request.get(FilterKey("resize-charge"));
      use->enableRoutedAccounting(0, tracker, "segment 0");
      {
        auto claim = use->probe(0);
        ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
        cache.resize(bytes);
        EXPECT_THROW(use->publishRaw(0, claim, bitDocs(4096, 7), 1), ResourceExhaustedError);
      }
      EXPECT_EQ(nullptr, use->rawDocSet(0));
      EXPECT_NE(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
    }
    EXPECT_EQ(0u, tracker.bytes());
    cache.validateForTest();
  }
}

TEST(FilterCacheTest, resizeZeroRacingInsertCleansWithoutAnotherRequest) {
  for (bool allocated : {false, true}) {
    FilterCache cache({.maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 1});
    std::array segments{FilterCache::SegmentIdentity{1, 4096}};
    cache.onReaderPublished(1, segments);
    if (allocated) {
      for (int i = 0; i < 128; i++) {
        servePolicyValue(cache, segments, FilterKey("seed:" + std::to_string(i)), i, 10);
      }
      cache.clear();
    }
    std::latch inserting(1), resume(1);
    Signal::listen("filterCacheBeforeInsert", [&](void* object, void*, void*) -> void* {
      if (object == &cache) { inserting.count_down(); resume.wait(); }
      return nullptr;
    });
    std::thread worker([&] {
      FilterCache::UseRegistry request(cache, 1, segments);
      request.get(FilterKey("late-insert"));
    });
    inserting.wait();
    cache.resize(0);
    resume.count_down();
    worker.join();
    Signal::unlisten("filterCacheBeforeInsert");
    EXPECT_EQ(0u, cache.bytesUsed());
    EXPECT_EQ(0u, cache.metadataBytesUsed());
    EXPECT_EQ(0u, cache.entryCountForTest());
    cache.validateForTest();
  }
}

TEST(FilterCacheTest, resizeZeroRacingReaderInsertCleansWithoutAnotherRequest) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir));
  IndexWriter writer(snapshots);
  addTermDoc(writer, "body");
  writer.commit();
  auto reader = snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  for (bool allocated : {false, true}) {
    FilterCache cache({.maxBytes = 1024 * 1024, .admissionThreshold = 1});
    cache.onReaderPublished(*reader);
    if (allocated) {
      for (int i = 0; i < 128; i++) {
        FilterCache::UseRegistry request(cache, *reader);
        request.get(FilterKey("seed:" + std::to_string(i)), FilterKeyScope::READER_STABLE)
            ->probeReaderStable(*reader, domains);
      }
      cache.clear();
    }
    std::latch inserting(1), resume(1);
    Signal::listen("filterCacheBeforeInsert", [&](void*, void*, void*) -> void* {
      inserting.count_down(); resume.wait(); return nullptr;
    });
    std::thread worker([&] {
      FilterCache::UseRegistry request(cache, *reader);
      auto* use = request.get(FilterKey("late-reader"), FilterKeyScope::READER_STABLE);
      EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
                use->probeReaderStable(*reader, domains).kind());
    });
    inserting.wait();
    cache.resize(0);
    resume.count_down();
    worker.join();
    Signal::unlisten("filterCacheBeforeInsert");
    EXPECT_EQ(0u, cache.bytesUsed());
    EXPECT_EQ(0u, cache.metadataBytesUsed());
    EXPECT_EQ(0u, cache.entryCountForTest());
    cache.validateForTest();
  }
}

TEST(FilterCacheTest, resizeZeroDetachesValuePublishedBeforeInsertCheck) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir));
  IndexWriter writer(snapshots);
  // Two docs: a segment value matching every doc is never populated.
  addTermDoc(writer, "body");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  for (auto scope : {FilterKeyScope::SEGMENT_STABLE, FilterKeyScope::READER_STABLE}) {
    FilterCache cache({.maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 1});
    cache.onReaderPublished(*reader);
    std::latch inserted(1), check(1), disabled(1), clear(1);
    Signal::listen("filterCacheEntryInserted", [&](void*, void*, void*) -> void* {
      inserted.count_down(); check.wait(); return nullptr;
    });
    Signal::listen("filterCacheDisabled", [&](void*, void*, void*) -> void* {
      disabled.count_down(); clear.wait(); return nullptr;
    });
    std::thread inserter([&] {
      FilterCache::UseRegistry request(cache, *reader);
      auto* use = request.get(FilterKey("insert-check"), scope);
      if (scope == FilterKeyScope::READER_STABLE) use->probeReaderStable(*reader, domains);
    });
    inserted.wait();
    FilterCache::UseRegistry borrower(cache, *reader);
    auto* use = borrower.get(FilterKey("insert-check"), scope);
    DocSet* pin;
    if (scope == FilterKeyScope::READER_STABLE) {
      auto claim = use->probeReaderStable(*reader, domains);
      EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD, claim.kind());
      auto value = use->publishReaderStable(claim, oneDocPerSegment(*reader), 10);
      pin = value->docSet(0, readerIdentitiesForTest(*reader)[0]);
    } else {
      auto claim = use->probe(0);
      EXPECT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
      pin = use->publishRaw(0, claim, docs(reader->segments()[0].maxDoc(), {0}), 10)->docSet();
    }
    EXPECT_GT(cache.bytesUsed(), 0u);
    std::thread disabler([&] { cache.resize(0); });
    disabled.wait();
    check.count_down();
    inserter.join();
    EXPECT_EQ(0u, cache.bytesUsed());
    EXPECT_EQ(0u, cache.metadataBytesUsed());
    EXPECT_EQ(0u, cache.entryCountForTest());
    EXPECT_TRUE(pin->get(0));
    clear.count_down();
    disabler.join();
    Signal::unlisten("filterCacheEntryInserted");
    Signal::unlisten("filterCacheDisabled");
    cache.validateForTest();
  }
}

TEST(FilterCacheTest, resizeShrinkHandsOffToActiveSweeper) {
  FilterCache cache({.maxBytes = 1024 * 1024, .minSegmentDocs = 0,
                     .admissionThreshold = 1, .maxMetadataBytes = 64 * 1024});
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  for (int i = 0; i < 8; i++) {
    servePolicyValue(cache, segments, FilterKey("handoff:" + std::to_string(i)), i, 10);
  }
  std::latch elected(1), resume(1);
  Signal::listen("filterCacheSweepStarted", [&](void*, void*, void*) -> void* {
    elected.count_down(); resume.wait(); return nullptr;
  });
  std::thread sweeper([&] { cache.sweep(); });
  elected.wait();
  cache.resize(cache.bytesUsed() / 2);
  resume.count_down();
  sweeper.join();
  Signal::unlisten("filterCacheSweepStarted");
  EXPECT_LE(cache.bytesUsed(), cache.configuration().maxBytes);
  cache.validateForTest();
}

TEST(FilterCacheTest, concurrentResizeAndRoutedPublish) {
  FilterCache cache({.maxBytes = 1024 * 1024, .minSegmentDocs = 0,
                     .admissionThreshold = 1, .maxMetadataBytes = 64 * 1024});
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  std::latch start(1);
  std::vector<std::thread> threads;
  for (int tid = 0; tid < 6; tid++) {
    threads.emplace_back([&, tid] {
      start.wait();
      for (int i = 0; i < 500; i++) {
        RequestMemTracker tracker(1);
        {
          FilterCache::UseRegistry request(cache, 1, segments);
          auto* use = request.get(FilterKey("cycling:" + std::to_string((i + tid) % 64)));
          use->enableRoutedAccounting(0, tracker, "segment 0");
          {
            auto claim = use->probe(0);
            if (claim.kind() == FilterCache::Probe::Kind::BUILD) {
              try { use->publishRaw(0, claim, bitDocs(4096, tid), 10); }
              catch (const ResourceExhaustedError&) { EXPECT_EQ(nullptr, use->rawDocSet(0)); }
            }
          }
        }
        EXPECT_EQ(0u, tracker.bytes());
      }
    });
  }
  start.count_down();
  for (int i = 0; i < 500; i++) {
    for (size_t bytes : {0u, 64u * 1024, 1024u * 1024, 0u, 8u * 1024}) cache.resize(bytes);
  }
  // Disable while publishers may still hold Uses; check their automatic cleanup.
  cache.resize(0);
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_EQ(0u, cache.metadataBytesUsed());
  EXPECT_EQ(0u, cache.entryCountForTest());
  cache.validateForTest();
}

TEST(FilterCacheTest, segmentLookIsInertOnMissingAndRetiredValues) {
  FilterCache cache(testConfig());
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("unseen-segment");
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  auto before = cache.counters();
  for (int i = 0; i < 4; i++) EXPECT_FALSE(use->look(0));
  EXPECT_EQ(before, cache.counters());
  EXPECT_FALSE(use->wasAdmitted());
  EXPECT_EQ(0u, cache.entryCountForTest());

  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache evicting(config);
  std::array largeSegments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(evicting.onReaderPublished(1, largeSegments));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(evicting, largeSegments, key, 1, 1));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(evicting, largeSegments, FilterKey("winner"), 2, 100'000));
  FilterCache::UseRegistry retired(evicting, 1, largeSegments);
  auto* ghost = retired.get(key);
  before = evicting.counters();
  for (int i = 0; i < 4; i++) EXPECT_FALSE(ghost->look(0));
  EXPECT_EQ(before, evicting.counters());
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, ghost->probe(0).kind());
  }
  evicting.resize(charge * 2);
  FilterCache::UseRegistry next(evicting, 1, largeSegments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, next.get(key)->probe(0).kind());
}

TEST(FilterCacheTest, segmentCandidatePinsAcrossDetachAndDeduplicatesAcceptance) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("looked-value");
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, servePolicyValue(cache, segments, key, 7, 1));
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  auto before = cache.counters();
  auto dump = cache.dump(1, nullptr);
  auto candidate = use->look(0);
  ASSERT_TRUE(candidate);
  EXPECT_EQ(1, candidate->card());
  EXPECT_EQ(before, cache.counters());
  auto looked = cache.dump(1, nullptr);
  ASSERT_EQ(1u, looked.size());
  EXPECT_EQ(dump[0].lastUsed, looked[0].lastUsed);
  EXPECT_EQ(dump[0].values[0].recent, looked[0].values[0].recent);
  DocSet* docs = candidate->docSet();
  std::thread evict([&] {
    servePolicyValue(cache, segments, FilterKey("replacement"), 11, 100'000);
  });
  evict.join();
  ASSERT_EQ(1u, cache.counters().unrepaidEvictions);
  EXPECT_TRUE(docs->get(7));
  before = cache.counters();
  auto hit = use->accept(std::move(*candidate));
  EXPECT_EQ(docs, hit.docSet());
  EXPECT_EQ(before.hits + 1, cache.counters().hits);
  EXPECT_EQ(before.estimatedSavedNanos + 1, cache.counters().estimatedSavedNanos);
  before = cache.counters();
  auto second = use->look(0);
  ASSERT_TRUE(second);
  EXPECT_EQ(docs, use->probe(0).docSet());
  EXPECT_EQ(docs, use->accept(std::move(*second)).docSet());
  EXPECT_EQ(before, cache.counters());
  cache.resize(charge * 2);
  FilterCache::UseRegistry next(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, next.get(key)->probe(0).kind());
  cache.validateForTest();
}

TEST(FilterCacheTest, residentFilterModesKeepTheirDensityBands) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(snapshots);
  for (int i = 0; i < 1024; i++) {
    std::string text = "other";
    if (i < 1) text += " sparse";
    if (i < 2) text += " middle";
    if (i < 4) text += " dense";
    addTermDoc(writer, text);
  }
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto* cache = reader->filterCache();
  auto& segment = reader->segments()[0];
  for (auto term : {"sparse", "middle", "dense"}) {
    MemPool pool;
    Query::Context context(pool, *reader);
    TermQuery query("text_w", term);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query);
    auto probe = use->probe(0);
    use->publishRaw(0, probe, QueryPrep::materializeRawFilter(*weight, nullptr, segment), 10);
  }
  for (auto mode : {QueryPrep::FilterSupplierMode::DENSITY_ROUTED,
                    QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE,
                    QueryPrep::FilterSupplierMode::SPARSE_BATCH}) {
    for (auto term : {"sparse", "middle", "dense"}) {
      SCOPED_TRACE(term);
      SCOPED_TRACE((int)mode);
      MemPool pool;
      Query::Context context(pool, *reader);
      TermQuery query("text_w", term);
      auto* weight = query.createWeight(context, 0);
      auto* use = context.getFilterUse(query);
      auto before = cache->counters();
      auto dump = cache->dump(3, nullptr);
      bool admitted = use->wasAdmitted();
      auto* supplier = QueryPrep::filterSupplier(pool, *weight, use, *reader, segment, mode, 1024);
      bool cached = mode == QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE
          || std::string_view(term) == "dense"
          || (mode == QueryPrep::FilterSupplierMode::SPARSE_BATCH && std::string_view(term) == "sparse");
      EXPECT_EQ(cached, dynamic_cast<QueryPrep::DocSetSupplier*>(supplier) != nullptr);
      EXPECT_EQ(admitted, use->wasAdmitted());
      if (!cached) {
        EXPECT_EQ(before, cache->counters());
        auto after = cache->dump(3, nullptr);
        ASSERT_EQ(dump.size(), after.size());
        for (size_t i = 0; i < dump.size(); i++) {
          EXPECT_EQ(dump[i].lastUsed, after[i].lastUsed);
          EXPECT_EQ(dump[i].values[0].recent, after[i].values[0].recent);
        }
      }
    }
  }
}

TEST(FilterCacheTest, prohibitedResidentDensityRejectionDoesNotAccept) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(snapshots);
  for (int i = 0; i < 1024; i++) addTermDoc(writer, i == 0 ? "sparse" : "other");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  MemPool pool;
  Query::Context context(pool, *reader);
  TermQuery query("text_w", "sparse");
  QueryPrep::PreparedSource source;
  source.weight = query.createWeight(context, 0);
  source.cacheUse = context.getFilterUse(query);
  auto probe = source.cacheUse->probe(0);
  source.cacheUse->publishRaw(0, probe, QueryPrep::materializeRawFilter(*source.weight, nullptr, segment), 10);
  auto* cache = reader->filterCache();
  auto before = cache->counters();
  auto* supplier = QueryPrep::prohibitedSupplier(pool, source, *reader, segment, true,
      QueryPrep::ProhibitedCacheRoute::DENSITY);
  ASSERT_NE(nullptr, supplier);
  EXPECT_EQ(before, cache->counters());
  auto planContext = supplier->makePlanContext(Query::Demand{});
  EXPECT_EQ(Query::DirectScorerKind::TERM, supplier->describeScorer(planContext).directKind);
}

TEST(FilterCacheTest, existingLookupIsInertUntilCompleteAcceptance) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{
    FilterCache::SegmentIdentity{1, 100},
    FilterCache::SegmentIdentity{2, 100},
  };
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("complete-resident");

  {
    FilterCache::UseRegistry populate(cache, 1, segments);
    auto* use = populate.get(key);
    for (size_t i = 0; i < segments.size(); i++) {
      auto probe = use->probe(i);
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
      use->publishRaw(i, probe, docs(100, {(int32_t)i + 3}), 10);
    }
  }

  auto before = cache.counters();
  FilterCache::UseRegistry request(cache, 1, segments);
  auto candidate = request.lookupExisting(key);
  ASSERT_TRUE(candidate.has_value());
  EXPECT_EQ(0u, request.size());
  auto afterLookup = cache.counters();
  EXPECT_EQ(before.hits, afterLookup.hits);
  EXPECT_EQ(before.misses, afterLookup.misses);
  EXPECT_EQ(before.admissions, afterLookup.admissions);
  EXPECT_EQ(before.buildAttempts, afterLookup.buildAttempts);

  auto* acceptedUse = request.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, acceptedUse);
  EXPECT_EQ(1u, request.size());
  EXPECT_EQ(before.hits + segments.size(), cache.counters().hits);
  for (size_t i = 0; i < segments.size(); i++) {
    auto probe = acceptedUse->probe(i);
    ASSERT_EQ(FilterCache::Probe::Kind::HIT, probe.kind());
    ASSERT_NE(nullptr, probe.docSet());
    EXPECT_TRUE(probe.docSet()->get((int32_t)i + 3));
  }
  // Request-local serving after acceptance does not double-count shared hits.
  EXPECT_EQ(before.hits + segments.size(), cache.counters().hits);
}

TEST(FilterCacheTest, existingAcceptanceRevalidatesEverySegment) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 2;
  FilterCache cache(config);
  std::array oldSegments{
    FilterCache::SegmentIdentity{1, 100},
    FilterCache::SegmentIdentity{2, 100},
  };
  ASSERT_TRUE(cache.onReaderPublished(1, oldSegments));
  FilterKey key("stale-complete-resident");
  {
    FilterCache::UseRegistry first(cache, 1, oldSegments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
              first.get(key)->probe(0).kind());
  }
  {
    FilterCache::UseRegistry populate(cache, 1, oldSegments);
    auto* use = populate.get(key);
    for (size_t i = 0; i < oldSegments.size(); i++) {
      auto probe = use->probe(i);
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
      use->publishRaw(i, probe, docs(100, {(int32_t)i + 7}), 10);
    }
  }

  FilterCache::UseRegistry request(cache, 1, oldSegments);
  auto candidate = request.lookupExisting(key);
  ASSERT_TRUE(candidate.has_value());
  auto before = cache.counters();
  std::array newSegments{FilterCache::SegmentIdentity{2, 100}};
  ASSERT_TRUE(cache.onReaderPublished(2, newSegments));
  EXPECT_EQ(nullptr, request.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE));
  EXPECT_EQ(0u, request.size());
  EXPECT_EQ(before.hits, cache.counters().hits);
  FilterCache::UseRegistry firstWhole(cache, 2, newSegments);
  EXPECT_FALSE(firstWhole.get(
      key, FilterKeyScope::SEGMENT_STABLE,
      FilterCache::AdmissionLane::WHOLE)->wasAdmitted());
}

TEST(FilterCacheTest, existingReaderAcceptanceIsInertUntilCommit) {
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
  IndexWriter writer(writerSnapshots);
  addTermDoc(writer, "first");
  addTermDoc(writer, "second");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);

  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  ASSERT_TRUE(cache.onReaderPublished(*reader));
  FilterKey key("existing-reader-resident");
  {
    FilterCache::UseRegistry populate(cache, *reader);
    auto* use = populate.get(
        key, FilterKeyScope::READER_STABLE,
        FilterCache::AdmissionLane::CLAUSE);
    auto probe = use->probeReaderStable(*reader, domains);
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
    use->publishReaderStable(
        probe, oneDocPerSegment(*reader), 10);
  }

  auto before = cache.counters();
  FilterCache::UseRegistry request(cache, *reader);
  auto candidate = request.lookupExisting(
      key, FilterKeyScope::READER_STABLE);
  ASSERT_TRUE(candidate.has_value());
  EXPECT_EQ(0u, request.size());
  auto afterLookup = cache.counters();
  EXPECT_EQ(before.hits, afterLookup.hits);
  EXPECT_EQ(before.readerStableHits, afterLookup.readerStableHits);
  EXPECT_EQ(before.misses, afterLookup.misses);
  EXPECT_EQ(before.admissions, afterLookup.admissions);
  EXPECT_EQ(before.buildAttempts, afterLookup.buildAttempts);

  auto* acceptedUse = request.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, acceptedUse);
  EXPECT_TRUE(acceptedUse->hasAcceptedExisting());
  EXPECT_EQ(before.hits + 1, cache.counters().hits);
  EXPECT_EQ(before.readerStableHits + 1,
            cache.counters().readerStableHits);
  for (auto& segment : reader->segments()) {
    DomainHandle accepted = acceptedUse->effectiveDomain(
        (size_t) segment.ord, *reader,
        domains[(size_t) segment.ord]);
    ASSERT_NE(nullptr, accepted.get());
    EXPECT_EQ(1, accepted.get()->card());
  }
}

TEST(FilterCacheTest, existingReaderAcceptanceRejectsPublishedReader) {
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
  IndexWriter writer(writerSnapshots);
  addTermDoc(writer, "first");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);

  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  ASSERT_TRUE(cache.onReaderPublished(*reader));
  FilterKey key("existing-reader-stale");
  {
    FilterCache::UseRegistry populate(cache, *reader);
    auto* use = populate.get(
        key, FilterKeyScope::READER_STABLE,
        FilterCache::AdmissionLane::CLAUSE);
    auto probe = use->probeReaderStable(*reader, domains);
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
    use->publishReaderStable(probe, oneDocPerSegment(*reader), 10);
  }

  FilterCache::UseRegistry request(cache, *reader);
  auto candidate = request.lookupExisting(
      key, FilterKeyScope::READER_STABLE);
  ASSERT_TRUE(candidate.has_value());

  addTermDoc(writer, "second");
  writer.commit();
  auto nextReader = writer.snapshots.readers.getReader();
  ASSERT_GT(nextReader->commitTime(), reader->commitTime());
  ASSERT_TRUE(cache.onReaderPublished(*nextReader));
  auto beforeAccept = cache.counters();

  EXPECT_EQ(nullptr, request.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE));
  EXPECT_EQ(0u, request.size());
  auto afterAccept = cache.counters();
  EXPECT_EQ(beforeAccept.hits, afterAccept.hits);
  EXPECT_EQ(beforeAccept.readerStableHits, afterAccept.readerStableHits);
  EXPECT_EQ(beforeAccept.admissions, afterAccept.admissions);
}

TEST(FilterCacheTest, incompleteExistingLookupHasNoCacheEffects) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 2;
  FilterCache cache(config);
  std::array segments{
    FilterCache::SegmentIdentity{1, 100},
    FilterCache::SegmentIdentity{2, 100},
  };
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("partially-resident");
  {
    FilterCache::UseRegistry first(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
              first.get(key)->probe(0).kind());
  }
  {
    FilterCache::UseRegistry populate(cache, 1, segments);
    auto* use = populate.get(key);
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, docs(100, {9}), 10);
  }

  auto before = cache.counters();
  FilterCache::UseRegistry request(cache, 1, segments);
  EXPECT_FALSE(request.lookupExisting(key).has_value());
  EXPECT_EQ(0u, request.size());
  auto after = cache.counters();
  EXPECT_EQ(before.hits, after.hits);
  EXPECT_EQ(before.misses, after.misses);
  EXPECT_EQ(before.admissions, after.admissions);
  EXPECT_EQ(before.buildAttempts, after.buildAttempts);
  FilterCache::UseRegistry firstWhole(cache, 1, segments);
  EXPECT_FALSE(firstWhole.get(
      key, FilterKeyScope::SEGMENT_STABLE,
      FilterCache::AdmissionLane::WHOLE)->wasAdmitted());
}

TEST(FilterCacheTest, existingAcceptancePreservesLaneAdmission) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 2;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("lane-specific-resident");

  {
    FilterCache::UseRegistry firstClause(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
              firstClause.get(key)->probe(0).kind());
  }
  {
    FilterCache::UseRegistry secondClause(cache, 1, segments);
    auto* use = secondClause.get(key);
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, docs(100, {5}), 10);
  }

  FilterCache::UseRegistry firstWhole(cache, 1, segments);
  auto firstCandidate = firstWhole.lookupExisting(key);
  ASSERT_TRUE(firstCandidate.has_value());
  auto* firstUse = firstWhole.acceptExisting(
      std::move(*firstCandidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, firstUse);
  EXPECT_FALSE(firstUse->wasAdmitted());

  FilterCache::UseRegistry secondWhole(cache, 1, segments);
  auto secondCandidate = secondWhole.lookupExisting(key);
  ASSERT_TRUE(secondCandidate.has_value());
  auto* secondUse = secondWhole.acceptExisting(
      std::move(*secondCandidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, secondUse);
  EXPECT_TRUE(secondUse->wasAdmitted());
}

TEST(FilterCacheTest, existingAcceptancePaysDetachedInvestment) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey candidateKey("detached-candidate");
  FilterKey replacementKey("high-priority-replacement");

  {
    FilterCache::UseRegistry populate(cache, 1, segments);
    auto* use = populate.get(candidateKey);
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, bitDocs(4096, 7), 1);
  }
  FilterCache::UseRegistry request(cache, 1, segments);
  auto candidate = request.lookupExisting(candidateKey);
  ASSERT_TRUE(candidate.has_value());

  {
    FilterCache::UseRegistry replacement(cache, 1, segments);
    auto* use = replacement.get(replacementKey);
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, bitDocs(4096, 11), 100'000);
  }
  ASSERT_EQ(1u, cache.counters().unrepaidEvictions);

  auto* acceptedUse = request.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, acceptedUse);
  auto accepted = acceptedUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, accepted.kind());
  ASSERT_NE(nullptr, accepted.docSet());
  EXPECT_TRUE(accepted.docSet()->get(7));

  cache.resize(charge * 2);
  FilterCache::UseRegistry retry(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            retry.get(candidateKey)->probe(0).kind());
}

TEST(FilterCacheTest, admissionLanesIsolateScanTraffic) {
  auto verify = [](FilterCache::AdmissionLane seededLane,
                   FilterCache::AdmissionLane floodLane) {
    FilterCacheConfig config = testConfig();
    config.admissionHistorySize = 4;
    config.wholeAdmissionHistorySize = 4;
    config.admissionThreshold = 2;
    FilterCache cache(config);
    std::array segments{FilterCache::SegmentIdentity{1, 100}};
    ASSERT_TRUE(cache.onReaderPublished(1, segments));
    FilterKey recurring = FilterKey::withHashForTest("recurring", 7);

    {
      FilterCache::UseRegistry seed(cache, 1, segments);
      EXPECT_FALSE(seed.get(recurring, FilterKeyScope::SEGMENT_STABLE,
                            seededLane)->wasAdmitted());
    }
    for (uint64_t i = 0; i < 32; i++) {
      FilterCache::UseRegistry flood(cache, 1, segments);
      auto key = FilterKey::withHashForTest(
          "flood-" + std::to_string(i), 100 + i);
      flood.get(key, FilterKeyScope::SEGMENT_STABLE, floodLane);
    }

    FilterCache::UseRegistry second(cache, 1, segments);
    auto* use = second.get(recurring, FilterKeyScope::SEGMENT_STABLE,
                           seededLane);
    EXPECT_TRUE(use->wasAdmitted());
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD, use->probe(0).kind());
  };

  verify(FilterCache::AdmissionLane::CLAUSE,
         FilterCache::AdmissionLane::WHOLE);
  verify(FilterCache::AdmissionLane::WHOLE,
         FilterCache::AdmissionLane::CLAUSE);
}

TEST(FilterCacheTest, admissionLanePromotionDeduplicatesEachRequest) {
  FilterCacheConfig config = testConfig();
  config.admissionHistorySize = 8;
  config.wholeAdmissionHistorySize = 8;
  config.admissionThreshold = 2;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey clauseFirst("clause-first");
  FilterKey wholeFirst("whole-first");

  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(clauseFirst);
    EXPECT_EQ(use, request.get(clauseFirst));
    EXPECT_EQ(use, request.get(clauseFirst, FilterKeyScope::SEGMENT_STABLE,
                               FilterCache::AdmissionLane::WHOLE));
    EXPECT_FALSE(use->wasAdmitted());

    auto* reverse = request.get(
        wholeFirst, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::WHOLE);
    EXPECT_EQ(reverse, request.get(
        wholeFirst, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::CLAUSE));
    EXPECT_FALSE(reverse->wasAdmitted());
    EXPECT_EQ(0u, cache.entryCountForTest());
  }

  FilterCache::UseRegistry second(cache, 1, segments);
  auto* fromWhole = second.get(
      clauseFirst, FilterKeyScope::SEGMENT_STABLE,
      FilterCache::AdmissionLane::WHOLE);
  EXPECT_TRUE(fromWhole->wasAdmitted());
  auto* fromClause = second.get(
      wholeFirst, FilterKeyScope::SEGMENT_STABLE,
      FilterCache::AdmissionLane::CLAUSE);
  EXPECT_TRUE(fromClause->wasAdmitted());
  EXPECT_NE(fromWhole->entryIdentityForTest(),
            fromClause->entryIdentityForTest());
  EXPECT_EQ(2u, cache.entryCountForTest());
}

TEST(FilterCacheTest, wholeLanePopulatesBelowMinimumSegmentDocs) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.minSegmentDocs = 1000;
  FilterCache cache(config);
  std::array segments{
    FilterCache::SegmentIdentity{1, 1000},
    FilterCache::SegmentIdentity{2, 100},
  };
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("whole-tiny-segment");

  {
    FilterCache::UseRegistry populate(cache, 1, segments);
    auto* use = populate.get(
        key, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::WHOLE);
    auto large = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, large.kind());
    use->publishRaw(0, large, docs(1000, {3}), 10);
    auto tiny = use->probe(1);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, tiny.kind());
    use->publishRaw(1, tiny, docs(100, {7}), 10);
  }
  EXPECT_EQ(2u, cache.counters().builds);

  FilterCache::UseRegistry hit(cache, 1, segments);
  auto candidate = hit.lookupExisting(key);
  ASSERT_TRUE(candidate.has_value());
  auto* use = hit.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, use);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
  auto tinyHit = use->probe(1);
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, tinyHit.kind());
  ASSERT_NE(nullptr, tinyHit.docSet());
  EXPECT_TRUE(tinyHit.docSet()->get(7));
}

TEST(FilterCacheTest, clauseLaneDoesNotPopulateBelowMinimumSegmentDocs) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.minSegmentDocs = 1000;
  FilterCache cache(config);
  std::array segments{
    FilterCache::SegmentIdentity{1, 1000},
    FilterCache::SegmentIdentity{2, 100},
  };
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("clause-tiny-segment");

  {
    FilterCache::UseRegistry populate(cache, 1, segments);
    auto* use = populate.get(
        key, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::CLAUSE);
    auto large = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, large.kind());
    use->publishRaw(0, large, docs(1000, {3}), 10);
    auto tiny = use->probe(1);
    ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, tiny.kind());
    use->publishRaw(1, tiny, docs(100, {7}), 10);
  }
  EXPECT_EQ(1u, cache.counters().builds);

  FilterCache::UseRegistry verify(cache, 1, segments);
  EXPECT_FALSE(verify.lookupExisting(key).has_value());
  auto* use = verify.get(
      key, FilterKeyScope::SEGMENT_STABLE,
      FilterCache::AdmissionLane::CLAUSE);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(1).kind());
}

TEST(FilterCacheTest, clauseEntryUpgradesTinySegmentsAfterWholeAdmission) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 2;
  config.minSegmentDocs = 1000;
  FilterCache cache(config);
  std::array segments{
    FilterCache::SegmentIdentity{1, 1000},
    FilterCache::SegmentIdentity{2, 100},
  };
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("clause-then-whole-tiny-segment");

  {
    FilterCache::UseRegistry firstClause(cache, 1, segments);
    auto* use = firstClause.get(
        key, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::CLAUSE);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0).kind());
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(1).kind());
  }
  {
    FilterCache::UseRegistry secondClause(cache, 1, segments);
    auto* use = secondClause.get(
        key, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::CLAUSE);
    auto large = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, large.kind());
    use->publishRaw(0, large, docs(1000, {3}), 10);
    auto tiny = use->probe(1);
    ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, tiny.kind());
    use->publishRaw(1, tiny, docs(100, {7}), 10);
  }
  {
    FilterCache::UseRegistry firstWhole(cache, 1, segments);
    EXPECT_FALSE(firstWhole.lookupExisting(key).has_value());
    auto* use = firstWhole.get(
        key, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::WHOLE);
    EXPECT_FALSE(use->wasAdmitted());
    EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
    auto tiny = use->probe(1);
    ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, tiny.kind());
  }
  {
    FilterCache::UseRegistry secondWhole(cache, 1, segments);
    auto* use = secondWhole.get(
        key, FilterKeyScope::SEGMENT_STABLE,
        FilterCache::AdmissionLane::WHOLE);
    EXPECT_TRUE(use->wasAdmitted());
    EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
    auto tiny = use->probe(1);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, tiny.kind());
    use->publishRaw(1, tiny, docs(100, {7}), 10);
  }

  FilterCache::UseRegistry hit(cache, 1, segments);
  auto candidate = hit.lookupExisting(key);
  ASSERT_TRUE(candidate.has_value());
  auto* use = hit.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, use);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(1).kind());
}

TEST(FilterCacheTest, publishHitsEmptyAndForcesCardinality) {
  FilterCache cache(testConfig());
  std::array segments{FilterCache::SegmentIdentity{7, 1024}};
  cache.onReaderPublished(3, segments);
  FilterKey key("empty-result");

  FilterCache::UseRegistry first(cache, 3, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
            first.get(key)->probe(0).kind());
  FilterCache::UseRegistry second(cache, 3, segments);
  auto* building = second.get(key);
  auto probe = building->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
  EXPECT_EQ(0u, cache.counters().builds);
  auto value = building->publishRaw(0, probe, docs(1024, {}), 1);
  EXPECT_EQ(1u, cache.counters().builds);
  ASSERT_NE(nullptr, value);
  EXPECT_EQ(0, value->card());
  EXPECT_EQ(0, value->docSet()->cachedCard());

  FilterCache::UseRegistry third(cache, 3, segments);
  auto hit = third.get(key)->probe(0);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, hit.kind());
  EXPECT_EQ(0, hit.card());

  FilterKey lazyCardKey("lazy-card");
  FilterCache::UseRegistry lazyFirst(cache, 3, segments);
  EXPECT_FALSE(lazyFirst.get(lazyCardKey)->wasAdmitted());
  FilterCache::UseRegistry lazy(cache, 3, segments);
  auto* lazyUse = lazy.get(lazyCardKey);
  ASSERT_TRUE(lazyUse->wasAdmitted());
  auto lazyProbe = lazyUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, lazyProbe.kind());
  auto lazyValue = lazyUse->publishRaw(0, lazyProbe, bitDocs(1024, 42), 1);
  EXPECT_EQ(1, lazyValue->card());
  EXPECT_EQ(1, lazyValue->docSet()->cachedCard());
}

TEST(FilterCacheTest, builderClaimIsNonblockingAndExceptionSafe) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("claim");

  try {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto claim = request.get(key)->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
    FilterCache::UseRegistry racer(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
              racer.get(key)->probe(0).kind());
    throw std::runtime_error("materialization failed");
  } catch (const std::runtime_error&) {
  }

  FilterCache::UseRegistry retry(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            retry.get(key)->probe(0).kind());
  EXPECT_EQ(0u, cache.counters().builds);
  EXPECT_EQ(2u, cache.counters().buildAttempts);
}

TEST(FilterCacheTest, publicationRevalidatesPurgeCoreAndIdentity) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array oldSegments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(4, oldSegments);
  FilterKey key("purge-race");
  FilterCache::UseRegistry oldRequest(cache, 4, oldSegments);
  auto* oldUse = oldRequest.get(key);
  auto claim = oldUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());

  std::array newSegments{FilterCache::SegmentIdentity{2, 100}};
  cache.onReaderPublished(5, newSegments);
  auto local = oldUse->publishRaw(0, claim, docs(100, {3, 7}), 1);
  EXPECT_EQ(2, local->card());
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_GE(cache.counters().publishRejects, 1u);
  EXPECT_FALSE(cache.onReaderPublished(3, oldSegments));

  std::array mismatch{FilterCache::SegmentIdentity{2, 101}};
  FilterCache::UseRegistry wrongSize(cache, 5, mismatch);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
            wrongSize.get(key)->probe(0).kind());
  auto outside = oldUse->offerRaw(1, docs(100, {9}), 1);
  EXPECT_TRUE(outside->docSet()->get(9));
  EXPECT_EQ(0u, cache.bytesUsed());
}

TEST(FilterCacheTest, routedAccountingOwnsRejectedSegmentValue) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array oldSegments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(4, oldSegments);
  RequestMemTracker tracker(0);

  {
    FilterCache::UseRegistry request(cache, 4, oldSegments);
    auto* use = request.get(FilterKey("routed-stale-segment"));
    use->enableRoutedAccounting(0, tracker, "segment 0");
    auto claim = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());

    std::array newSegments{FilterCache::SegmentIdentity{2, 100}};
    cache.onReaderPublished(5, newSegments);
    auto value = use->publishRaw(
        0, claim, docs(100, {3, 7}), 1);
    EXPECT_EQ(value->ramBytesUsed(), tracker.bytes());
    EXPECT_EQ(0u, cache.bytesUsed());
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST(FilterCacheTest, rejectedChargeOfRejectedPublishLeavesNoRawValue) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array oldSegments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(4, oldSegments);
  RequestMemTracker tracker(1);

  {
    FilterCache::UseRegistry request(cache, 4, oldSegments);
    auto* use = request.get(FilterKey("routed-over-limit"));
    use->enableRoutedAccounting(0, tracker, "segment 0");
    {
      // As in a request, unwinding releases the build claim.
      auto claim = use->probe(0);
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
      std::array newSegments{FilterCache::SegmentIdentity{2, 100}};
      cache.onReaderPublished(5, newSegments);
      EXPECT_ANY_THROW(use->publishRaw(0, claim, docs(100, {3, 7}), 1));
    }
    // The only owner of the rejected value unwound with the throw.
    EXPECT_EQ(nullptr, use->rawDocSet(0));
    EXPECT_NE(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST(FilterCacheTest, oldSchemaPopulationCannotHitNewSchemaKey) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(4, segments);
  TermQuery query("text_w", "value");

  FilterCache::UseRegistry oldRequest(cache, 4, segments);
  auto* oldUse = oldRequest.get(keyFor(query, 10));
  auto oldClaim = oldUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, oldClaim.kind());

  // The schema swap changes only the key envelope; an already-running old
  // request may still finish against the same reader snapshot.
  FilterCache::UseRegistry newRequest(cache, 4, segments);
  auto* newUse = newRequest.get(keyFor(query, 11));
  oldUse->publishRaw(0, oldClaim, docs(100, {3, 7}), 1);

  EXPECT_NE(FilterCache::Probe::Kind::HIT, newUse->probe(0).kind());
}

TEST(FilterCacheTest, byproductOfferIsFirstWins) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{3, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("byproduct");
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* cacheUse = request.get(key);

  auto first = cacheUse->offerRaw(0, docs(100, {1, 2}), 1);
  auto second = cacheUse->offerRaw(0, docs(100, {9}), 1);
  EXPECT_EQ(2, first->card());
  EXPECT_EQ(2, second->card());
  EXPECT_EQ(1u, cache.counters().byproductInserts);

  FilterCache::UseRegistry next(cache, 1, segments);
  auto hit = next.get(key)->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, hit.kind());
  EXPECT_TRUE(hit.docSet()->get(1));
  EXPECT_FALSE(hit.docSet()->get(9));
}

TEST(FilterCacheTest, fullBytesDefeatMapHashCollision) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey first = FilterKey::withHashForTest("first", 99);
  FilterKey second = FilterKey::withHashForTest("second", 99);

  FilterCache::UseRegistry request(cache, 1, segments);
  auto* firstUse = request.get(first);
  auto firstProbe = firstUse->probe(0);
  firstUse->publishRaw(0, firstProbe, docs(100, {1}), 1);
  auto* secondUse = request.get(second);
  auto secondProbe = secondUse->probe(0);
  secondUse->publishRaw(0, secondProbe, docs(100, {2}), 1);
  EXPECT_EQ(2u, cache.entryCountForTest());

  FilterCache::UseRegistry hits(cache, 1, segments);
  auto firstHit = hits.get(first)->probe(0);
  auto secondHit = hits.get(second)->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, firstHit.kind());
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, secondHit.kind());
  EXPECT_TRUE(firstHit.docSet()->get(1));
  EXPECT_FALSE(firstHit.docSet()->get(2));
  EXPECT_TRUE(secondHit.docSet()->get(2));
}

TEST(FilterCacheTest, zeroBuildCostStaysZero) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("zero-cost");
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, key, 1, 0));
  EXPECT_EQ(FilterCache::Probe::Kind::HIT,
            servePolicyValue(cache, segments, key, 1, 0));
  auto rows = cache.dump(10, nullptr);
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ(0u, rows[0].buildNanos);
  EXPECT_EQ(0u, cache.counters().buildNanos);
  EXPECT_EQ(0u, cache.counters().estimatedSavedNanos);
}

TEST(FilterCacheTest, entryCapRejectsAndMatchAllDoesNotPopulate) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = FilterKey("large").bytes().size();
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* tooLarge = request.get(FilterKey("large"));
  auto tooLargeProbe = tooLarge->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, tooLargeProbe.kind());
  tooLarge->publishRaw(0, tooLargeProbe, docs(100, {1}), 123);
  EXPECT_EQ(123u, cache.counters().buildNanos);
  EXPECT_EQ(0u, cache.bytesUsed());

  config.maxEntryBytes = 1024 * 1024;
  FilterCache allCache(config);
  allCache.onReaderPublished(1, segments);
  FilterCache::UseRegistry allRequest(allCache, 1, segments);
  auto* allUse = allRequest.get(FilterKey("all"));
  auto allProbe = allUse->probe(0);
  DocSetBuilder builder(100);
  for (int32_t i = 0; i < 100; i++) builder.add(i);
  allUse->publishRaw(0, allProbe, builder.build(), 1);
  EXPECT_EQ(0u, allCache.bytesUsed());

  config.minSegmentDocs = 101;
  FilterCache tinyCache(config);
  tinyCache.onReaderPublished(1, segments);
  FilterCache::UseRegistry tinyRequest(tinyCache, 1, segments);
  tinyRequest.get(FilterKey("tiny"))->offerRaw(0, docs(100, {1, 2}), 1);
  EXPECT_EQ(0u, tinyCache.bytesUsed());
  EXPECT_EQ(0u, tinyCache.counters().buildNanos);
}

TEST(FilterCacheTest, evictsLowerRecentWorkAndPinsRetiredValue) {
  auto sample = bitDocs(1024, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 16;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey first("cheap");
  FilterKey second("expensive");
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* firstUse = request.get(first);
  auto firstProbe = firstUse->probe(0, FilterCache::Work::nanoseconds(10));
  auto pinned = firstUse->publishRaw(
      0, firstProbe, std::move(sample), 1000);
  auto* secondUse = request.get(second);
  auto secondProbe = secondUse->probe(0, FilterCache::Work::nanoseconds(10000));
  secondUse->publishRaw(0, secondProbe, bitDocs(1024, 2), 1000);

  EXPECT_EQ(charge, cache.bytesUsed());
  EXPECT_EQ(1u, cache.counters().evictions);
  EXPECT_TRUE(pinned->docSet()->get(1));
  FilterCache::UseRegistry verify(cache, 1, segments);
  EXPECT_NE(FilterCache::Probe::Kind::HIT, verify.get(first)->probe(0).kind());
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, verify.get(second)->probe(0).kind());
  ASSERT_NO_THROW(cache.validateForTest());
}

namespace {
void evictValues(FilterCache& cache) {
  size_t budget = cache.configuration().maxBytes;
  cache.resize(1);
  cache.sweep();
  cache.resize(budget);
}

FilterCache::ValueDump ledgerFor(FilterCache& cache, const FilterKey& key) {
  auto rows = cache.dump(100, nullptr, true);
  for (const auto& row : rows) {
    if (row.keyHash == key.hash() && !row.values.empty()) return row.values[0];
  }
  ADD_FAILURE() << "missing ledger";
  return {};
}
}

namespace {
void advanceCacheActivity(FilterCache& cache,
    std::span<const FilterCache::SegmentIdentity> segments, size_t count) {
  for (size_t i = 0; i < count; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(FilterKey("activity"));
  }
}

std::shared_ptr<const FilterCache::SegmentValue> publishWork(FilterCache& cache,
    std::span<const FilterCache::SegmentIdentity> segments, const FilterKey& key,
    double work, uint64_t production = 1000) {
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  auto probe = use->probe(0, work == 0 ? FilterCache::Work::assumed()
      : FilterCache::Work::nanoseconds(work));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
  if (probe.kind() != FilterCache::Probe::Kind::BUILD) return {};
  return use->publishRaw(0, probe, bitDocs(segments[0].maxDoc, 7), production);
}
}

TEST(FilterCacheTest, recentWorkCountsAbsentAndResidentPaymentsOnce) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 8;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey key("continuous-work");
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto probe = use->probe(0, FilterCache::Work::pending());
    ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, probe.kind());
    use->charge(0, FilterCache::Work::nanoseconds(4000));
    use->charge(0, FilterCache::Work::nanoseconds(4000));
  }
  EXPECT_EQ(0u, cache.lastRefreshEpoch());
  advanceCacheActivity(cache, segments, 8);
  EXPECT_EQ(0u, cache.lastRefreshEpoch());
  cache.sweep();
  auto before = ledgerFor(cache, key);
  EXPECT_EQ(4000, before.recent);
  EXPECT_GE(before.recentEpochs, 8u);
  auto firstEpoch = cache.lastRefreshEpoch();
  cache.sweep();
  EXPECT_EQ(firstEpoch, cache.lastRefreshEpoch());
  EXPECT_EQ(4000, ledgerFor(cache, key).recent);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto probe = use->probe(0, FilterCache::Work::pending());
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, bitDocs(1024, 7), 10000);
  }
  EXPECT_EQ(before.recent, ledgerFor(cache, key).recent);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0, FilterCache::Work::nanoseconds(3000)).kind());
    EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0, FilterCache::Work::nanoseconds(3000)).kind());
  }
  advanceCacheActivity(cache, segments, 8);
  cache.sweep();
  auto resident = ledgerFor(cache, key);
  EXPECT_EQ(3000, resident.recent);
  EXPECT_EQ(3000, resident.creditedTotal);
  EXPECT_EQ(7000, resident.debt);
  auto epoch = cache.lastRefreshEpoch();
  cache.resetCounters();
  EXPECT_EQ(epoch, cache.lastRefreshEpoch());
  EXPECT_EQ(3000, ledgerFor(cache, key).recent);
  advanceCacheActivity(cache, segments, 8);
  cache.sweep();
  EXPECT_EQ(0, ledgerFor(cache, key).recent);
  EXPECT_TRUE(ledgerFor(cache, key).resident);
  cache.resize(bitDocs(1024, 7)->ramBytesUsed());
  publishWork(cache, segments, FilterKey("young-value"), 0);
  EXPECT_FALSE(ledgerFor(cache, key).resident);
  EXPECT_TRUE(ledgerFor(cache, FilterKey("young-value")).resident);
}

TEST(FilterCacheTest, oneLargeSavingOutranksManyCheapServes) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 8;
  size_t charge = bitDocs(1024, 7)->ramBytesUsed();
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey large("large-saving"), small("small-saving");
  publishWork(cache, segments, large, 0);
  publishWork(cache, segments, small, 0);
  for (int i = 0; i < 20; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(small)->probe(0, FilterCache::Work::nanoseconds(1));
  }
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(large)->probe(0, FilterCache::Work::nanoseconds(10000));
  }
  publishWork(cache, segments, FilterKey("new-saving"), 20000);
  EXPECT_TRUE(ledgerFor(cache, large).resident);
  EXPECT_FALSE(ledgerFor(cache, small).resident);
  EXPECT_EQ(1u, cache.counters().evictions);
  EXPECT_EQ(11000, ledgerFor(cache, large).recent);
}

TEST(FilterCacheTest, absentWorkRanksANewValueInTheFreshList) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 64;
  size_t charge = bitDocs(1024, 7)->ramBytesUsed();
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey funded("funded-before-production");
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(funded);
    auto probe = use->probe(0, FilterCache::Work::pending());
    use->charge(0, FilterCache::Work::nanoseconds(10000));
  }
  advanceCacheActivity(cache, segments, 64);
  cache.sweep();
  auto epoch = cache.lastRefreshEpoch();
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(funded);
    auto probe = use->probe(0, FilterCache::Work::pending());
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, bitDocs(1024, 7), 1000);
  }
  publishWork(cache, segments, FilterKey("not-yet-recent"), 0);
  EXPECT_EQ(epoch, cache.lastRefreshEpoch());
  EXPECT_TRUE(ledgerFor(cache, funded).resident);
  EXPECT_FALSE(ledgerFor(cache, FilterKey("not-yet-recent")).resident);
}

TEST(FilterCacheTest, detachedCreditRepaysWithoutRaisingReplacementWork) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 8;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey key("late-credit-work");
  auto old = publishWork(cache, segments, key, 0);
  FilterCache::UseRegistry late(cache, 1, segments);
  auto* use = late.get(key);
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0, FilterCache::Work::pending()).kind());
  use->credit(0, FilterCache::Work::nanoseconds(300));
  FilterCache::UseRegistry later(cache, 1, segments);
  auto* laterUse = later.get(key);
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, laterUse->probe(0, FilterCache::Work::pending()).kind());
  evictValues(cache);
  EXPECT_EQ(700, ledgerFor(cache, key).debt);
  EXPECT_EQ(1300, ledgerFor(cache, key).recent);
  publishWork(cache, segments, key, 1700, 2000);
  laterUse->credit(0, FilterCache::Work::nanoseconds(200));
  laterUse->credit(0, FilterCache::Work::nanoseconds(200));
  EXPECT_EQ(2500, ledgerFor(cache, key).debt);
  EXPECT_EQ(0, ledgerFor(cache, key).creditedTotal);
  advanceCacheActivity(cache, segments, 8);
  cache.sweep();
  EXPECT_EQ(1700, ledgerFor(cache, key).recent);
  EXPECT_TRUE(old->docSet()->get(7));
}

TEST(FilterCacheTest, payloadSweepFreesOnlyTheExcess) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  size_t charge = bitDocs(1024, 7)->ramBytesUsed();
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 50 + charge / 2;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  for (int i = 0; i < 51; i++) publishWork(cache, segments, FilterKey("value-" + std::to_string(i)), 0);
  EXPECT_EQ(1u, cache.counters().evictions);
  EXPECT_EQ(50 * charge, cache.bytesUsed());
  cache.resize(35 * charge + charge / 2);
  EXPECT_EQ(16u, cache.counters().evictions);
  EXPECT_EQ(35 * charge, cache.bytesUsed());
  cache.resize(60 * charge);
  EXPECT_EQ(16u, cache.counters().evictions);
  EXPECT_EQ(35 * charge, cache.bytesUsed());
  cache.validateForTest();
}

TEST(FilterCacheTest, metadataOnlySweepKeepsFittingPayloads) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.maxMetadataEntries = 1;
  size_t charge = bitDocs(1024, 7)->ramBytesUsed();
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge + 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  publishWork(cache, segments, FilterKey("resident"), 0);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(FilterKey("metadata-only"));
  }
  cache.sweep();
  EXPECT_EQ(0u, cache.counters().evictions);
  EXPECT_EQ(charge, cache.bytesUsed());
  EXPECT_EQ(1u, cache.entryCountForTest());
  cache.validateForTest();
}

namespace {
FilterCache::Probe::Kind observedRequest(FilterCache& cache,
    std::span<const FilterCache::SegmentIdentity> segments, const FilterKey& key,
    uint64_t nanos, std::optional<uint64_t> buildNanos = std::nullopt, bool completed = true,
    FilterCache::Work work = FilterCache::Work::pending()) {
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key, FilterKeyScope::SEGMENT_STABLE,
      work.isWholeConsumer() ? FilterCache::AdmissionLane::WHOLE : FilterCache::AdmissionLane::CLAUSE);
  auto probe = use->probe(0, work);
  auto kind = probe.kind();
  if (buildNanos) {
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD, kind);
    if (kind == FilterCache::Probe::Kind::BUILD) {
      use->publishRaw(0, probe, docs(segments[0].maxDoc, {7}), *buildNanos);
    }
  }
  request.addSegmentNanos(0, nanos);
  request.settle(completed);
  request.settle(completed);
  return kind;
}
}

namespace {
FilterCacheConfig capacityConfig(size_t bytes, uint64_t window = 1024) {
  auto config = testConfig();
  config.maxBytes = bytes;
  config.maxMetadataBytes = 64 << 20;
  config.maxMetadataEntries = 4096;
  config.admissionThreshold = 1;
  config.recentWorkEpochs = window;
  return config;
}

void seedCapacityValue(FilterCache& cache,
    std::span<const FilterCache::SegmentIdentity> segments, const FilterKey& key,
    double credit, uint64_t production = 1000, bool trainPrior = false,
    std::unique_ptr<DocSet> raw = {}) {
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto probe = use->probe(0, trainPrior ? FilterCache::Work::nanoseconds(0) : FilterCache::Work::assumed());
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, raw ? std::move(raw) : bitDocs(segments[0].maxDoc, 7), production);
  }
  if (credit > 0) {
    FilterCache::UseRegistry request(cache, 1, segments);
    ASSERT_EQ(FilterCache::Probe::Kind::HIT,
        request.get(key)->probe(0, FilterCache::Work::nanoseconds(credit)).kind());
  }
}

void fundCapacityValue(FilterCache& cache,
    std::span<const FilterCache::SegmentIdentity> segments, const FilterKey& key, double work) {
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
  use->charge(0, FilterCache::Work::nanoseconds(work));
}

class CapacityChecks {
  FilterCache& cache;
public:
  int count = 0;
  explicit CapacityChecks(FilterCache& cache) : cache(cache) {
    Signal::listen("filterCacheCapacityCompared", [this](void* object, void*, void*) -> void* {
      if (object == &this->cache) count++;
      return nullptr;
    });
  }
  ~CapacityChecks() { Signal::unlisten("filterCacheCapacityCompared"); }
};
}

TEST(FilterCacheTest, capacityGateStartsAtShrinkAndEndsAtGrow) {
  size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(bytes * 2));
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  seedCapacityValue(cache, segments, FilterKey("resident"), 10000, 1000, true);
  FilterKey candidate("priced-candidate");
  fundCapacityValue(cache, segments, candidate, 1000);
  CapacityChecks checks(cache);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD, request.get(candidate)->probe(0, FilterCache::Work::pending()).kind());
  }
  EXPECT_EQ(0, checks.count);
  cache.resize(bytes);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, request.get(candidate)->probe(0, FilterCache::Work::pending()).kind());
  }
  EXPECT_EQ(1, checks.count);
  cache.resize(bytes * 2);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD, request.get(candidate)->probe(0, FilterCache::Work::pending()).kind());
  }
  EXPECT_EQ(1, checks.count);
  FilterCache::UseRegistry assumed(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, assumed.get(FilterKey("assumed"))->probe(0).kind());
}

TEST(FilterCacheTest, capacityEqualityAndSmallAdvantagesKeepTheResident) {
  for (double work : {1050, 1100, 1101}) {
    SCOPED_TRACE(work);
    size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
    FilterCache cache(capacityConfig(bytes));
    std::array segments{FilterCache::SegmentIdentity{1, 1024}};
    cache.onReaderPublished(1, segments);
    seedCapacityValue(cache, segments, FilterKey("resident"), 100, 1000, true);
    FilterKey candidate("candidate");
    fundCapacityValue(cache, segments, candidate, work);
    auto before = cache.counters();
    FilterCache::UseRegistry request(cache, 1, segments);
    auto result = request.get(candidate)->probe(0, FilterCache::Work::pending());
    EXPECT_EQ(work > 1100 ? FilterCache::Probe::Kind::BUILD : FilterCache::Probe::Kind::BYPASS, result.kind());
    EXPECT_EQ(before.populationDeferred, cache.counters().populationDeferred);
    EXPECT_EQ(before.capacityDeferred + (work <= 1100), cache.counters().capacityDeferred);
    EXPECT_EQ(work > 1100, ledgerFor(cache, FilterKey("resident")).spokenFor);
  }
}

TEST(FilterCacheTest, capacityRefusalMemoAvoidsListWorkUntilDemandOrRefresh) {
  size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(bytes, 64));
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  seedCapacityValue(cache, segments, FilterKey("resident"), 2000, 1000, true);
  FilterKey candidate("equal-key");
  fundCapacityValue(cache, segments, candidate, 2000);
  CapacityChecks checks(cache);
  auto before = cache.counters();
  for (int i = 0; i < 3; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(candidate);
    for (int j = 0; j < 3; j++) {
      EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
    }
  }
  EXPECT_EQ(1, checks.count);
  EXPECT_EQ(before.capacityDeferred + 3, cache.counters().capacityDeferred);
  EXPECT_EQ(before.populationDeferred, cache.counters().populationDeferred);
  advanceCacheActivity(cache, segments, 64);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(candidate);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
    use->charge(0, FilterCache::Work::nanoseconds(20000));
  }
  EXPECT_EQ(2, checks.count);
  FilterCache::UseRegistry request(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, request.get(candidate)->probe(0, FilterCache::Work::pending()).kind());
  EXPECT_EQ(3, checks.count);
}

TEST(FilterCacheTest, knownCardinalityFitsWithoutDisplacingPayloads) {
  constexpr int32_t maxDoc = 65536;
  size_t small = docs(maxDoc, {1, 2, 3, 4})->ramBytesUsed();
  size_t big = bitDocs(maxDoc, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(big + small));
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  FilterKey resident("resident"), candidate("candidate");
  seedCapacityValue(cache, segments, resident, 10000, 1000, true);
  fundCapacityValue(cache, segments, candidate, 1000);
  CapacityChecks checks(cache);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(candidate);
  ASSERT_TRUE(use->recordCardinality(0, 4));
  auto grant = use->probe(0, FilterCache::Work::pending());
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, grant.kind());
  EXPECT_EQ(0, checks.count);
  use->publishRaw(0, grant, docs(maxDoc, {1, 2, 3, 4}), 1000);
  EXPECT_EQ(big + small, cache.bytesUsed());
  EXPECT_TRUE(ledgerFor(cache, resident).resident);
  EXPECT_EQ(0u, cache.counters().evictions);
}

TEST(FilterCacheTest, learnedSizeRechecksRefusalWithoutMoreWorkOrARefresh) {
  constexpr int32_t maxDoc = 65536;
  size_t small = docs(maxDoc, {1, 2, 3, 4})->ramBytesUsed();
  size_t big = bitDocs(maxDoc, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(big + small));
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  FilterKey cheap("cheap-victim"), costly("costly-victim"), candidate("candidate");
  seedCapacityValue(cache, segments, cheap, 100, 1000, true, docs(maxDoc, {1, 2, 3, 4}));
  seedCapacityValue(cache, segments, costly, 10000);
  fundCapacityValue(cache, segments, candidate, 2000);
  CapacityChecks checks(cache);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(candidate);
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
  }
  EXPECT_EQ(1, checks.count);
  uint64_t rolled = cache.lastRefreshEpoch();
  auto before = cache.counters();
  ASSERT_TRUE(use->recordCardinality(0, 4));
  auto grant = use->probe(0, FilterCache::Work::pending());
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, grant.kind());
  EXPECT_EQ(2, checks.count);
  EXPECT_EQ(rolled, cache.lastRefreshEpoch());
  EXPECT_EQ(before.observedCharges, cache.counters().observedCharges);
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, candidate).owed);
  EXPECT_TRUE(ledgerFor(cache, cheap).spokenFor);
  EXPECT_FALSE(ledgerFor(cache, costly).spokenFor);
  use->publishRaw(0, grant, docs(maxDoc, {1, 2, 3, 4}), 1000);
  EXPECT_FALSE(ledgerFor(cache, cheap).resident);
  EXPECT_TRUE(ledgerFor(cache, costly).resident);
  EXPECT_TRUE(ledgerFor(cache, candidate).resident);
  EXPECT_EQ(big + small, cache.bytesUsed());
}

TEST(FilterCacheTest, measuredFootprintTakesPrecedenceOverCardinalityEstimate) {
  constexpr int32_t maxDoc = 65536;
  size_t small = docs(maxDoc, {1, 2, 3, 4})->ramBytesUsed();
  size_t big = bitDocs(maxDoc, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(big + small));
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  FilterKey candidate("candidate");
  seedCapacityValue(cache, segments, candidate, 1000, 1000, true);
  evictValues(cache);
  ASSERT_EQ(big, ledgerFor(cache, candidate).referenceBytes);
  seedCapacityValue(cache, segments, FilterKey("cheap-victim"), 100, 1000, false,
      docs(maxDoc, {1, 2, 3, 4}));
  seedCapacityValue(cache, segments, FilterKey("costly-victim"), 10000);
  fundCapacityValue(cache, segments, candidate, 2000);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(candidate);
  EXPECT_EQ(1, use->cardinality(0));
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
}

TEST(FilterCacheTest, observedBuildIsTheOnlyUseInItsWindowAndKeepsItsRank) {
  constexpr int32_t maxDoc = 1024;
  size_t bytes = bitDocs(maxDoc, 7)->ramBytesUsed();
  auto config = capacityConfig(bytes * 2, 64);
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  FilterKey low("low-demand"), built("observed-build"), next("new-demand");
  seedCapacityValue(cache, segments, low, 0, 1000, true);
  auto work = FilterCache::Work::pending().forWholeQuery();
  ASSERT_EQ(FilterCache::Probe::Kind::BYPASS,
      observedRequest(cache, segments, built, 1000, {}, true, work));
  advanceCacheActivity(cache, segments, 64);
  cache.sweep();
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(built, FilterKeyScope::SEGMENT_STABLE, FilterCache::AdmissionLane::WHOLE);
    auto grant = use->probe(0, work);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, grant.kind());
    use->publishRaw(0, grant, bitDocs(maxDoc, 7), 5000);
    EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0, work).kind());
    request.addSegmentNanos(0, 100000);
    request.settle(true);
  }
  EXPECT_EQ(1u, ledgerFor(cache, built).absentCount);
  EXPECT_EQ(0u, ledgerFor(cache, built).servedCount);
  EXPECT_EQ(0, ledgerFor(cache, built).creditedTotal);
  EXPECT_EQ(0, ledgerFor(cache, built).owed);
  EXPECT_EQ(5000, ledgerFor(cache, built).debt);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::HIT,
        request.get(low)->probe(0, FilterCache::Work::nanoseconds(100)).kind());
  }
  advanceCacheActivity(cache, segments, 64);
  cache.sweep();
  EXPECT_EQ(1000, ledgerFor(cache, built).recent);
  EXPECT_GT(ledgerFor(cache, built).rankedRate, ledgerFor(cache, low).rankedRate);
  publishWork(cache, segments, next, 10000);
  EXPECT_TRUE(ledgerFor(cache, built).resident);
  EXPECT_FALSE(ledgerFor(cache, low).resident);
  cache.validateForTest();
}

TEST(FilterCacheTest, assumedBuildContributesOnceWithoutAHit) {
  constexpr int32_t maxDoc = 1024;
  size_t bytes = bitDocs(maxDoc, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(bytes * 2, 64));
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  FilterKey low("low-demand"), built("assumed-build");
  seedCapacityValue(cache, segments, low, 100, 1000, true);
  publishWork(cache, segments, built, 0, 4000);
  advanceCacheActivity(cache, segments, 64);
  cache.sweep();
  EXPECT_EQ(4000, ledgerFor(cache, built).recent);
  EXPECT_EQ(0, ledgerFor(cache, built).creditedTotal);
  EXPECT_EQ(4000, ledgerFor(cache, built).debt);
  publishWork(cache, segments, FilterKey("new-demand"), 10000);
  EXPECT_TRUE(ledgerFor(cache, built).resident);
  EXPECT_FALSE(ledgerFor(cache, low).resident);
  evictValues(cache);
  publishWork(cache, segments, built, 9000, 5000);
  advanceCacheActivity(cache, segments, 64);
  cache.sweep();
  EXPECT_EQ(9000, ledgerFor(cache, built).recent);
  EXPECT_EQ(9000, ledgerFor(cache, built).debt);
  cache.validateForTest();
}

TEST(FilterCacheTest, readerBuildAddsDemandWithoutCreditAndByproductsDoNot) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir));
  IndexWriter writer(snapshots);
  addTermDoc(writer, "first");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  FilterCache cache(capacityConfig(1 << 20, 64));
  cache.onReaderPublished(*reader);
  FilterKey key("reader-build-demand"), byproduct("byproduct-demand");
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    auto grant = use->probeReaderStable(*reader, domains);
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, grant.kind());
    FilterCache::ReaderProbe moved;
    moved = std::move(grant);
    use->publishReaderStable(moved, oneDocPerSegment(*reader), 4000);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::HIT, use->probeReaderStable(*reader, domains).kind());
    request.get(byproduct)->offerRaw(0, docs(reader->segments()[0].maxDoc(), {0}), 9000);
  }
  advanceCacheActivity(cache, readerIdentitiesForTest(*reader), 64);
  cache.sweep();
  EXPECT_EQ(4000, ledgerFor(cache, key).recent);
  EXPECT_EQ(0, ledgerFor(cache, key).creditedTotal);
  EXPECT_EQ(4000, ledgerFor(cache, key).debt);
  EXPECT_EQ(0, ledgerFor(cache, byproduct).recent);
  EXPECT_EQ(0, ledgerFor(cache, byproduct).debt);
  cache.validateForTest();
}

TEST(FilterCacheTest, unknownWholeSizeMeasuresAtTheFirstFundedProbe) {
  constexpr int32_t maxDoc = 65536;
  auto config = capacityConfig(bitDocs(maxDoc, 7)->ramBytesUsed());
  config.admissionThreshold = 2;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  FilterKey resident("large-resident"), candidate("unknown-whole-size");
  {
    FilterCache::UseRegistry sighting(cache, 1, segments);
    sighting.get(resident);
  }
  seedCapacityValue(cache, segments, resident, 2000);
  auto whole = FilterCache::Work::pending().forWholeQuery();
  CapacityChecks checks(cache);
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
        observedRequest(cache, segments, candidate, 1000, {}, true, whole));
  }
  EXPECT_EQ(1u, ledgerFor(cache, candidate).absentCount);
  EXPECT_EQ(0, checks.count);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(candidate, FilterKeyScope::SEGMENT_STABLE, FilterCache::AdmissionLane::WHOLE);
  auto grant = use->probe(0, whole);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, grant.kind());
  EXPECT_EQ(0, checks.count);
  use->publishRaw(0, grant, docs(maxDoc, {7}), 800);
  EXPECT_FALSE(ledgerFor(cache, resident).resident);
  auto value = ledgerFor(cache, candidate);
  ASSERT_TRUE(value.resident);
  EXPECT_EQ(use->rawDocSet(0)->ramBytesUsed(), cache.bytesUsed());
  EXPECT_EQ(cache.bytesUsed(), value.referenceBytes);
  EXPECT_EQ(1, value.rawCardinality);
  EXPECT_EQ("measured", value.productionSource);
  EXPECT_DOUBLE_EQ(800, value.debt);
  EXPECT_DOUBLE_EQ(2000.0 / config.recentWorkEpochs, value.rankedRate);
  cache.validateForTest();
}

TEST(FilterCacheTest, wholeMeasurementCanBeEvictedOrRejectedAndIsNotFreeAgain) {
  constexpr int32_t maxDoc = 65536;
  for (bool rejected : {false, true}) {
    SCOPED_TRACE(rejected);
    size_t bytes = bitDocs(maxDoc, 7)->ramBytesUsed();
    FilterCache cache(capacityConfig(bytes));
    std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
    cache.onReaderPublished(1, segments);
    FilterKey resident("valuable-resident"), candidate("expensive-measurement");
    seedCapacityValue(cache, segments, resident, 100000);
    auto whole = FilterCache::Work::pending().forWholeQuery();
    observedRequest(cache, segments, candidate, 100, {}, true, whole);
    CapacityChecks checks(cache);
    auto before = cache.counters();
    {
      FilterCache::UseRegistry request(cache, 1, segments);
      auto* use = request.get(candidate, FilterKeyScope::SEGMENT_STABLE, FilterCache::AdmissionLane::WHOLE);
      auto grant = use->probe(0, whole);
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, grant.kind());
      DocSetBuilder raw(maxDoc);
      raw.addRange(0, rejected ? maxDoc : maxDoc / 2);
      use->publishRaw(0, grant, raw.build(), 1000);
      EXPECT_EQ(rejected ? maxDoc : maxDoc / 2, use->rawDocSet(0)->card());
    }
    EXPECT_EQ(0, checks.count);
    EXPECT_TRUE(ledgerFor(cache, resident).resident);
    auto ghost = ledgerFor(cache, candidate);
    EXPECT_FALSE(ghost.resident);
    EXPECT_EQ(bytes, ghost.referenceBytes);
    EXPECT_EQ("measured", ghost.productionSource);
    EXPECT_DOUBLE_EQ(1000, ghost.productionNanos);
    EXPECT_DOUBLE_EQ(1000, ghost.debt);
    EXPECT_DOUBLE_EQ(0, ghost.owed);
    EXPECT_EQ(before.buildAttempts + 1, cache.counters().buildAttempts);
    EXPECT_EQ(before.evictions + !rejected, cache.counters().evictions);
    EXPECT_EQ(before.publishRejects + rejected, cache.counters().publishRejects);
    for (int i = 0; i < 20; i++) {
      EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
          observedRequest(cache, segments, candidate, 100, {}, true, whole));
    }
    EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, candidate).owed);
    EXPECT_EQ(0, checks.count);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
        observedRequest(cache, segments, candidate, 100, {}, true, whole));
    EXPECT_EQ(1, checks.count);
    EXPECT_EQ(before.buildAttempts + 1, cache.counters().buildAttempts);
    cache.validateForTest();
  }
}

TEST(FilterCacheTest, knownCountsAndPriorPricedUsesStillCompareCapacity) {
  constexpr int32_t maxDoc = 65536;
  using Work = FilterCache::Work;
  for (Work work : {Work::pending(), Work::pending().forWholeQuery(false), Work::pending().forWholeQuery()}) {
    bool knownCount = work.isWholeQuery();
    SCOPED_TRACE(knownCount);
    FilterCache cache(capacityConfig(bitDocs(maxDoc, 7)->ramBytesUsed()));
    std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
    cache.onReaderPublished(1, segments);
    seedCapacityValue(cache, segments, FilterKey("valuable-resident"), 100000, 1000, true);
    FilterKey candidate("compared-candidate");
    observedRequest(cache, segments, candidate, 1000, {}, true, work);
    CapacityChecks checks(cache);
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(candidate, FilterKeyScope::SEGMENT_STABLE, FilterCache::AdmissionLane::WHOLE);
    if (knownCount) {
      ASSERT_TRUE(use->recordCardinality(0, 1));
    }
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, work).kind());
    EXPECT_EQ(1, checks.count);
  }
}

TEST(FilterCacheTest, concurrentUnknownWholeMeasurementsHaveOneCompletedProduction) {
  constexpr int32_t maxDoc = 1024;
  FilterCache cache(capacityConfig(bitDocs(maxDoc, 7)->ramBytesUsed()));
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  seedCapacityValue(cache, segments, FilterKey("valuable-resident"), 100000);
  FilterKey candidate("concurrent-measurement");
  auto whole = FilterCache::Work::pending().forWholeQuery();
  observedRequest(cache, segments, candidate, 1000, {}, true, whole);
  auto before = cache.counters();
  std::barrier phase(2);
  std::atomic<int> grants{0};
  auto claimant = [&] {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(candidate, FilterKeyScope::SEGMENT_STABLE, FilterCache::AdmissionLane::WHOLE);
    phase.arrive_and_wait();
    auto probe = use->probe(0, whole);
    if (probe.kind() == FilterCache::Probe::Kind::BUILD) grants.fetch_add(1);
    phase.arrive_and_wait();
    if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
      use->publishRaw(0, probe, bitDocs(maxDoc, 7), 1000);
    }
    phase.arrive_and_wait();
    FilterCache::UseRegistry after(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, after.get(candidate)->probe(0, whole).kind());
  };
  std::thread first(claimant), second(claimant);
  first.join(); second.join();
  EXPECT_EQ(1, grants.load());
  EXPECT_EQ(before.buildAttempts + 1, cache.counters().buildAttempts);
  EXPECT_FALSE(ledgerFor(cache, candidate).resident);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, candidate).debt);
  cache.validateForTest();
}

TEST(FilterCacheTest, capacityUsesOneWindowEvenAfterALateRoll) {
  for (bool late : {false, true}) {
    size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
    FilterCache cache(capacityConfig(bytes, late ? 16 : 1024));
    std::array segments{FilterCache::SegmentIdentity{1, 1024}};
    cache.onReaderPublished(1, segments);
    seedCapacityValue(cache, segments, FilterKey("resident"), 0, 1000, true);
    FilterKey candidate("not-extrapolated");
    fundCapacityValue(cache, segments, candidate, late ? 2000 : 1000);
    if (late) advanceCacheActivity(cache, segments, 64);
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, request.get(candidate)->probe(0, FilterCache::Work::pending()).kind());
    EXPECT_GT(cache.counters().capacityDeferred, 0u);
    if (late) { EXPECT_GT(ledgerFor(cache, candidate).recentEpochs, 16u); }
  }
}

TEST(FilterCacheTest, capacitySumsSmallVictimsAndCountsLargeVictimsWhole) {
  for (bool largeCandidate : {true, false}) {
    std::array segments{FilterCache::SegmentIdentity{1, 4096}};
    size_t small = docs(4096, {7})->ramBytesUsed();
    size_t big = bitDocs(4096, 7)->ramBytesUsed();
    FilterCache cache(capacityConfig(1 << 20));
    cache.onReaderPublished(1, segments);
    FilterKey candidate("candidate");
    if (largeCandidate) {
      cache.resize(small * 16);
      for (int i = 0; i < 16; i++) {
        seedCapacityValue(cache, segments, FilterKey("victim-" + std::to_string(i)),
            i == 0 ? 0 : 1000, 1000, i == 0, docs(4096, {7}));
      }
    } else {
      seedCapacityValue(cache, segments, candidate, 1000, 1000, true, docs(4096, {7}));
      evictValues(cache);
      cache.resize(big);
      seedCapacityValue(cache, segments, FilterKey("large-victim"), 10000);
    }
    size_t budget = cache.configuration().maxBytes;
    cache.resize(budget + 1);
    cache.resize(budget);
    fundCapacityValue(cache, segments, candidate, largeCandidate ? 3000 : 2000);
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(candidate);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
    if (largeCandidate) {
      use->charge(0, FilterCache::Work::nanoseconds(100000));
      auto grant = use->probe(0, FilterCache::Work::pending());
      EXPECT_EQ(FilterCache::Probe::Kind::BUILD, grant.kind());
      size_t covered = 0;
      for (const auto& row : cache.dump(100, nullptr)) if (row.values[0].spokenFor) covered += row.bytes;
      EXPECT_GE(covered, big);
      EXPECT_LT(covered - small, big);
    }
  }
}

TEST(FilterCacheTest, capacityMarksSurviveInsertionAndPrefixEviction) {
  size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
  size_t small = docs(1024, {7})->ramBytesUsed();
  FilterCache cache(capacityConfig(bytes * 2 + small + 1));
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey a("victim-a"), b("victim-b"), first("first"), second("second"), third("third");
  seedCapacityValue(cache, segments, a, 1000, 1000, true);
  seedCapacityValue(cache, segments, b, 5000);
  fundCapacityValue(cache, segments, first, 10000);
  fundCapacityValue(cache, segments, second, 20000);
  fundCapacityValue(cache, segments, third, 100000);
  FilterCache::UseRegistry r1(cache, 1, segments), r2(cache, 1, segments);
  auto p1 = r1.get(first)->probe(0, FilterCache::Work::pending());
  auto p2 = r2.get(second)->probe(0, FilterCache::Work::pending());
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, p1.kind());
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, p2.kind());
  {
    FilterCache::UseRegistry byproduct(cache, 1, segments);
    byproduct.get(FilterKey("tiny-idle"))->offerRaw(0, docs(1024, {7}), 1);
    byproduct.get(FilterKey("large-idle"))->offerRaw(0, bitDocs(1024, 7), 1);
  }
  EXPECT_TRUE(ledgerFor(cache, a).spokenFor);
  EXPECT_TRUE(ledgerFor(cache, b).spokenFor);
  FilterCache::UseRegistry r3(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, r3.get(third)->probe(0, FilterCache::Work::pending()).kind());
}

TEST(FilterCacheTest, capacityMarksOutliveThrowingAndAbandonedBuilds) {
  size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(bytes));
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey victim("victim"), candidate("candidate"), other("other");
  seedCapacityValue(cache, segments, victim, 0, 1000, true);
  fundCapacityValue(cache, segments, candidate, 10000);
  fundCapacityValue(cache, segments, other, 10000);
  EXPECT_THROW({
    FilterCache::UseRegistry request(cache, 1, segments);
    auto probe = request.get(candidate)->probe(0, FilterCache::Work::pending());
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    auto moved = std::move(probe);
    throw std::runtime_error("production failed");
  }, std::runtime_error);
  EXPECT_TRUE(ledgerFor(cache, victim).spokenFor);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, request.get(other)->probe(0, FilterCache::Work::pending()).kind());
  }
  cache.resize(bytes + 1);
  cache.resize(bytes);
  FilterCache::UseRegistry request(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, request.get(candidate)->probe(0, FilterCache::Work::pending()).kind());
  EXPECT_EQ(0, ledgerFor(cache, candidate).debt);
  EXPECT_EQ(10000, ledgerFor(cache, candidate).owed);
}

TEST(FilterCacheTest, assumedFirstBuildIsAnExperimentButRebuildIsCompared) {
  size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(bytes));
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey resident("resident"), unknown("unknown");
  seedCapacityValue(cache, segments, resident, 10000, 1000, true);
  seedCapacityValue(cache, segments, unknown, 0);
  EXPECT_FALSE(ledgerFor(cache, unknown).resident);
  EXPECT_EQ(0u, cache.counters().capacityDeferred);
  FilterCache::UseRegistry first(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, first.get(unknown)->probe(0).kind());
  FilterCache::UseRegistry next(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, next.get(unknown)->probe(0).kind());
  EXPECT_EQ(1u, cache.counters().capacityDeferred);
}

TEST(FilterCacheTest, capacityWorkingSetShiftAdmitsAfterIdleWindow) {
  size_t bytes = bitDocs(1024, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(bytes, 16));
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey candidate("new-working-set");
  seedCapacityValue(cache, segments, FilterKey("old-working-set"), 100000, 1000, true);
  fundCapacityValue(cache, segments, candidate, 10000);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, request.get(candidate)->probe(0, FilterCache::Work::pending()).kind());
  }
  for (int i = 0; i < 64; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(candidate);
    auto probe = use->probe(0, FilterCache::Work::pending());
    if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
      use->publishRaw(0, probe, bitDocs(1024, 7), 1000);
      EXPECT_TRUE(ledgerFor(cache, candidate).resident);
      return;
    }
    use->charge(0, FilterCache::Work::nanoseconds(1000));
  }
  FAIL() << "new working set did not replace idle resident";
}

TEST(FilterCacheTest, capacityCandidateMustOutrankItsVictims) {
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  size_t small = docs(4096, {7})->ramBytesUsed();
  size_t big = bitDocs(4096, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(big + small - 1));
  cache.onReaderPublished(1, segments);
  FilterKey dense("small-dense"), candidate("large-candidate");
  seedCapacityValue(cache, segments, dense, 5000, 1000, true, docs(4096, {7}));
  cache.resize(big + small);
  cache.resize(big + small - 1);
  // More work than the victim plus production, but less per byte than the victim.
  fundCapacityValue(cache, segments, candidate, 7000);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(candidate);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
  EXPECT_FALSE(ledgerFor(cache, dense).spokenFor);
  use->charge(0, FilterCache::Work::nanoseconds(100000));
  auto grant = use->probe(0, FilterCache::Work::pending());
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, grant.kind());
  use->publishRaw(0, grant, bitDocs(4096, 7), 1000);
  EXPECT_TRUE(ledgerFor(cache, candidate).resident);
  EXPECT_FALSE(ledgerFor(cache, dense).resident);
}

TEST(FilterCacheTest, capacityComparisonKeepsReaderValuesWhole) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir));
  IndexWriter writer(snapshots);
  addTermDoc(writer, "first"); addTermDoc(writer, "second"); writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  auto segments = readerIdentitiesForTest(*reader);
  FilterCache cache(capacityConfig(1 << 20));
  cache.onReaderPublished(*reader);
  auto seed = [&](const FilterKey& key, double credit) {
    {
      FilterCache::UseRegistry request(cache, *reader);
      auto* use = request.get(key, FilterKeyScope::READER_STABLE);
      auto probe = use->probeReaderStable(*reader, domains);
      ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
      use->publishReaderStable(probe, oneDocPerSegment(*reader), 1000);
    }
    FilterCache::UseRegistry request(cache, *reader);
    request.get(key, FilterKeyScope::READER_STABLE)->probeReaderStable(
        *reader, domains, FilterCache::Work::nanoseconds(credit));
  };
  FilterKey candidate("reader-candidate"), victim("reader-victim"), expensive("reader-expensive"), segment("segment");
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(segment);
    auto probe = use->probe(0, FilterCache::Work::nanoseconds(0));
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, docs(segments[0].maxDoc, {0}), 1000);
  }
  {
    FilterCache::UseRegistry request(cache, *reader);
    request.get(segment)->probe(0);
  }
  seed(candidate, 1000);
  size_t size = ledgerFor(cache, candidate).referenceBytes;
  evictValues(cache);
  cache.resize(size * 2);
  seed(victim, 1000); seed(expensive, 10000);
  cache.resize(size * 2 + 1);
  cache.resize(size * 2);
  EXPECT_GT(ledgerFor(cache, expensive).rankedRate, 0);
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(candidate, FilterKeyScope::READER_STABLE);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS, use->probeReaderStable(*reader, domains, FilterCache::Work::pending()).kind());
    use->chargeReaderStable(FilterCache::Work::nanoseconds(5000));
  }
  FilterCache::UseRegistry request(cache, *reader);
  auto* use = request.get(candidate, FilterKeyScope::READER_STABLE);
  auto grant = use->probeReaderStable(*reader, domains, FilterCache::Work::pending());
  ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, grant.kind());
  EXPECT_TRUE(ledgerFor(cache, victim).spokenFor);
  EXPECT_FALSE(ledgerFor(cache, expensive).spokenFor);
  use->publishReaderStable(grant, oneDocPerSegment(*reader), 1000);
  EXPECT_FALSE(ledgerFor(cache, victim).resident);
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* source = request.get(segment);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, source->probe(0, FilterCache::Work::pending()).kind());
    source->charge(0, FilterCache::Work::nanoseconds(2000));
  }
  FilterCache::UseRegistry smaller(cache, *reader);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, smaller.get(segment)->probe(0, FilterCache::Work::pending()).kind());
}

TEST(FilterCacheTest, capacityBoundsIrregularUniformProduction) {
  for (bool small : {false, true}) {
    int keys = small ? 1225 : 400, fit = small ? 50 : 47;
    uint64_t production = small ? 1000 : 17000, saving = 1000;
    size_t bytes = small ? docs(1024, {7})->ramBytesUsed() : bitDocs(1024, 7)->ramBytesUsed();
    FilterCache cache(capacityConfig(bytes * keys, 4096));
    std::array segments{FilterCache::SegmentIdentity{1, 1024}};
    cache.onReaderPublished(1, segments);
    std::vector<FilterKey> names;
    for (int i = 0; i < keys; i++) {
      names.emplace_back("uniform-key-" + std::to_string(i));
      seedCapacityValue(cache, segments, names.back(), production, production, false,
          small ? docs(1024, {7}) : bitDocs(1024, 7));
    }
    cache.resize(bytes * fit);
    for (int i = 0; i < 2; i++) { advanceCacheActivity(cache, segments, 4096); cache.sweep(); }
    cache.resetCounters();
    uint32_t random = 0x3267a9d1;
    for (int i = 0; i < 20000; i++) {
      random ^= random << 13; random ^= random >> 17; random ^= random << 5;
      FilterCache::UseRegistry request(cache, 1, segments);
      auto* use = request.get(names[random % keys]);
      auto probe = use->probe(0, FilterCache::Work::nanoseconds(saving));
      if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
        use->publishRaw(0, probe, small ? docs(1024, {7}) : bitDocs(1024, 7), production);
      }
    }
    auto counts = cache.counters();
    std::printf("capacity-replay small=%d hits=%llu builds=%llu build_ns=%llu saved_ns=%llu deferred=%llu\n",
        small, (unsigned long long)counts.hits, (unsigned long long)counts.builds,
        (unsigned long long)counts.buildNanos, (unsigned long long)counts.estimatedSavedNanos,
        (unsigned long long)counts.capacityDeferred);
    EXPECT_LE(counts.buildNanos, small ? 5000 * production : 3 * production);
    cache.validateForTest();
  }
}

TEST(FilterCacheTest, wholeObservationsUseOwnPriceAndDoNotTrainClausePrior) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey seed("clause-prior"), wholeKey("whole-price"), after("clause-after-whole");
  observedRequest(cache, segments, seed, 1);
  observedRequest(cache, segments, seed, 1, 1'000'000);
  auto whole = FilterCache::Work::pending().forWholeQuery();
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
      observedRequest(cache, segments, wholeKey, 400, {}, true, whole));
  auto price = ledgerFor(cache, wholeKey);
  EXPECT_EQ("absent", price.productionSource);
  EXPECT_DOUBLE_EQ(400, price.productionNanos);
  EXPECT_DOUBLE_EQ(400, price.owed);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
      observedRequest(cache, segments, wholeKey, 1, 100, true, whole));
  EXPECT_EQ(1u, ledgerFor(cache, wholeKey).absentCount);
  observedRequest(cache, segments, after, 1);
  EXPECT_EQ("prior", ledgerFor(cache, after).productionSource);
  EXPECT_DOUBLE_EQ(1'000'000, ledgerFor(cache, after).productionNanos);
  auto scoped = FilterCache::Work::pending().forWholeQuery(false);
  FilterKey scopedKey("scoped-whole");
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
      observedRequest(cache, segments, scopedKey, 400, {}, true, scoped));
  EXPECT_EQ("prior", ledgerFor(cache, scopedKey).productionSource);
  EXPECT_DOUBLE_EQ(1'000'000, ledgerFor(cache, scopedKey).productionNanos);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, observedRequest(cache, segments, scopedKey,
      1, 100, true, FilterCache::Work::nanoseconds(1'000'000).forWholeQuery(false)));
  FilterKey afterScoped("clause-after-scoped-whole");
  observedRequest(cache, segments, afterScoped, 1);
  EXPECT_DOUBLE_EQ(1'000'000, ledgerFor(cache, afterScoped).productionNanos);
}

TEST(FilterCacheTest, wholePriceBelongsToTheUseAndMixedPaymentsUseClausePricing) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey seed("basis-prior"), key("shared-basis");
  observedRequest(cache, segments, seed, 1);
  observedRequest(cache, segments, seed, 1, 1'000'000);
  auto whole = FilterCache::Work::pending().forWholeQuery();
  observedRequest(cache, segments, key, 100, {}, true, whole);
  EXPECT_EQ("absent", ledgerFor(cache, key).productionSource);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, segments, key, 100));
  EXPECT_EQ("prior", ledgerFor(cache, key).productionSource);
  EXPECT_DOUBLE_EQ(1'000'000, ledgerFor(cache, key).productionNanos);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
      observedRequest(cache, segments, key, 1, 100, true, whole));
  for (bool wholeFirst : {false, true}) {
    FilterKey mixed(wholeFirst ? "whole-first" : "clause-first");
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(mixed, FilterKeyScope::SEGMENT_STABLE, FilterCache::AdmissionLane::WHOLE);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
        use->probe(0, wholeFirst ? whole : FilterCache::Work::pending()).kind());
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
        use->probe(0, wholeFirst ? FilterCache::Work::pending() : whole).kind());
    request.addSegmentNanos(0, 100);
    request.settle(true);
    EXPECT_EQ(1u, ledgerFor(cache, mixed).absentCount);
    EXPECT_DOUBLE_EQ(100, ledgerFor(cache, mixed).owed);
    EXPECT_EQ("prior", ledgerFor(cache, mixed).productionSource);
  }
}

TEST(FilterCacheTest, wholeExperimentUsesMeasuredPriceAfterEviction) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("underpriced-whole");
  auto whole = FilterCache::Work::pending().forWholeQuery();
  observedRequest(cache, segments, key, 100, {}, true, whole);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
      observedRequest(cache, segments, key, 1, 10'000, true, whole));
  evictValues(cache);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
      observedRequest(cache, segments, key, 100, {}, true, whole));
  auto price = ledgerFor(cache, key);
  EXPECT_EQ("measured", price.productionSource);
  EXPECT_DOUBLE_EQ(10'000, price.productionNanos);
  EXPECT_DOUBLE_EQ(10'000, price.debt);
  EXPECT_DOUBLE_EQ(100, price.owed);
}

TEST(FilterCacheTest, completeWholeAcceptanceDefersCreditToSegmentTime) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("whole-preflight");
  auto whole = FilterCache::Work::pending().forWholeQuery();
  observedRequest(cache, segments, key, 1000, {}, true, whole);
  observedRequest(cache, segments, key, 1, 500, true, whole);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto candidate = request.lookupExisting(key);
  ASSERT_TRUE(candidate);
  ASSERT_NE(nullptr, request.acceptExisting(std::move(*candidate), FilterCache::AdmissionLane::WHOLE, whole));
  EXPECT_DOUBLE_EQ(500, ledgerFor(cache, key).debt);
  request.addSegmentNanos(0, 100);
  request.settle(true);
  request.settle(true);
  EXPECT_EQ(1u, ledgerFor(cache, key).servedCount);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).debt);
  EXPECT_EQ(900u, cache.counters().estimatedSavedNanos);
}

TEST(FilterCacheTest, firstObservedProductionWaitsForAnOrdinarySample) {
  FilterCache cache(testConfig());
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("observed-first");
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, segments, key, 400));
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, segments, key, 400));
  auto ledger = ledgerFor(cache, key);
  EXPECT_EQ(1u, ledger.absentCount);
  EXPECT_EQ("unknown", ledger.productionSource);
  EXPECT_DOUBLE_EQ(400, ledger.owed);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, observedRequest(cache, segments, key, 10'000, 1000));
  ledger = ledgerFor(cache, key);
  EXPECT_EQ(1u, ledger.absentCount);
  EXPECT_DOUBLE_EQ(0, ledger.owed);
  EXPECT_DOUBLE_EQ(1000, ledger.debt);
  EXPECT_EQ("measured", ledger.productionSource);
}

TEST(FilterCacheTest, observedPriorFundsPopulationAndLeavesAssumedConsumersUnchanged) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey seed("prior-seed"), key("prior-priced"), assumed("prior-assumed");
  observedRequest(cache, segments, seed, 100);
  observedRequest(cache, segments, seed, 100, 1000);
  std::array larger{FilterCache::SegmentIdentity{2, 200}};
  cache.onReaderPublished(1, larger);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, larger, key, 400));
  }
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, key).owed);
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, key).productionNanos);
  EXPECT_EQ("prior", ledgerFor(cache, key).productionSource);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, observedRequest(cache, larger, key, 400, 2000));
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, key).debt);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, servePolicyValue(cache, larger, assumed, 7, 500));
  FilterKey verify("prior-after-assumed-build");
  observedRequest(cache, larger, verify, 1);
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, verify).productionNanos);
}

TEST(FilterCacheTest, knownNanosecondsPriceBothProbeOutcomes) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("known-nanoseconds");
  servePolicyValue(cache, segments, key, 7, 1000);
  evictValues(cache);
  using Work = FilterCache::Work;
  for (double nanos : {800, 800, 400}) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto probe = use->probe(0, Work::nanoseconds(nanos));
    if (nanos == 400) {
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
      use->publishRaw(0, probe, docs(100, {7}), 2000);
    } else {
      ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, probe.kind());
      probe = {};
      EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, Work::nanoseconds(nanos)).kind());
    }
  }
  EXPECT_DOUBLE_EQ(3000, ledgerFor(cache, key).debt);
  FilterCache::UseRegistry request(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, request.get(key)->probe(0, Work::nanoseconds(250)).kind());
  EXPECT_DOUBLE_EQ(2750, ledgerFor(cache, key).debt);
  EXPECT_EQ(250u, cache.counters().estimatedSavedNanos);
  FilterKey next("prior-after-explicit-work");
  observedRequest(cache, segments, next, 1);
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, next).productionNanos);
}

TEST(FilterCacheTest, observedServesNeedABaselineAndSlowerSetsEarnNothing) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("observed-baseline");
  servePolicyValue(cache, segments, key, 7, 1000);
  observedRequest(cache, segments, key, 100);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  EXPECT_EQ(0u, ledgerFor(cache, key).absentCount);
  evictValues(cache);
  observedRequest(cache, segments, key, 400);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(key)->offerRaw(0, docs(100, {7}), 1);
  }
  observedRequest(cache, segments, key, 100);
  EXPECT_DOUBLE_EQ(700, ledgerFor(cache, key).debt);
  EXPECT_DOUBLE_EQ(300, ledgerFor(cache, key).creditedTotal);
  EXPECT_EQ(300u, cache.counters().estimatedSavedNanos);

  FilterKey slow("observed-slower");
  observedRequest(cache, segments, slow, 1000);
  observedRequest(cache, segments, slow, 1, 1000);
  observedRequest(cache, segments, slow, 2000);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, slow).debt);
  evictValues(cache);
  observedRequest(cache, segments, slow, 1000);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, slow).owed);
}

TEST(FilterCacheTest, matchedVariableAbsentAndServedMixHasNoResidualCharge) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("matched-variable-times");
  constexpr uint64_t low = 1'000'000, high = 100'000'000, production = 1'000'000'000;
  observedRequest(cache, segments, key, high);
  observedRequest(cache, segments, key, 1, production);
  observedRequest(cache, segments, key, high);
  for (int i = 0; i < 40; i++) {
    observedRequest(cache, segments, key, high);
    evictValues(cache);
    observedRequest(cache, segments, key, high);
    observedRequest(cache, segments, key, low);
    {
      FilterCache::UseRegistry request(cache, 1, segments);
      request.get(key)->offerRaw(0, docs(100, {7}), 1);
    }
    observedRequest(cache, segments, key, low);
  }
  auto ledger = ledgerFor(cache, key);
  EXPECT_DOUBLE_EQ(ledger.absentNanos, ledger.servedNanos);
  EXPECT_DOUBLE_EQ(0, ledger.owed);
  EXPECT_DOUBLE_EQ(production, ledger.debt);
  EXPECT_EQ(0u, cache.counters().estimatedSavedNanos);
}

TEST(FilterCacheTest, timedPassesSettleOnceAndFailedOrPreproductionRequestsTeachNothing) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("two-timed-passes");
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    for (int i = 0; i < 3; i++) use->probe(0, FilterCache::Work::pending());
    request.addSegmentNanos(0, 400);
    request.addSegmentNanos(0, 600);
    request.settle(true);
    request.settle(true);
  }
  EXPECT_EQ(1u, ledgerFor(cache, key).absentCount);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).owed);
  EXPECT_EQ(1u, cache.counters().observedCharges);
  observedRequest(cache, segments, key, 99'000, 1000);
  EXPECT_EQ(1u, ledgerFor(cache, key).absentCount);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).absentNanos);
  auto before = cache.counters();
  observedRequest(cache, segments, key, 100, std::nullopt, false);
  EXPECT_EQ(0u, ledgerFor(cache, key).servedCount);
  EXPECT_EQ(before.observedCredits, cache.counters().observedCredits);
  FilterKey aborted("aborted-ordinary");
  observedRequest(cache, segments, aborted, 1000, std::nullopt, false);
  EXPECT_EQ(0u, ledgerFor(cache, aborted).absentCount);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, aborted).owed);

  FilterKey stale("before-production");
  FilterCache::UseRegistry old(cache, 1, segments);
  old.get(stale)->probe(0, FilterCache::Work::pending());
  observedRequest(cache, segments, stale, 1000);
  observedRequest(cache, segments, stale, 1000, 1000);
  old.addSegmentNanos(0, 10'000);
  old.settle(true);
  EXPECT_EQ(1u, ledgerFor(cache, stale).absentCount);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, stale).owed);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, stale).debt);
  cache.sweep();
  EXPECT_EQ(2000, ledgerFor(cache, stale).recent);
  EXPECT_EQ(0, ledgerFor(cache, aborted).recent);
}

TEST(FilterCacheTest, observedDebtRequiresNetWorkAndNewProductionDoesNotRebaseIt) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("net-time-repayment");
  observedRequest(cache, segments, key, 400);
  observedRequest(cache, segments, key, 1, 1000);
  observedRequest(cache, segments, key, 100);
  EXPECT_DOUBLE_EQ(700, ledgerFor(cache, key).debt);
  evictValues(cache);
  for (int i = 0; i < 6; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, segments, key, 400));
  }
  EXPECT_DOUBLE_EQ(1800, ledgerFor(cache, key).owed);
  observedRequest(cache, segments, key, 1, 2000);
  EXPECT_DOUBLE_EQ(2700, ledgerFor(cache, key).debt);
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, key).productionNanos);
}

TEST(FilterCacheTest, onlyOutermostTimedScopeSelectsObservation) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(FilterKey("timed-intent"));
  use->observe();
  EXPECT_TRUE(use->work(0).isAssumed());
  {
    FilterCache::UseRegistry::SegmentScope outer(request, 0);
    EXPECT_FALSE(use->work(0).known());
    {
      FilterCache::UseRegistry::SegmentScope inner(request, 0);
      EXPECT_FALSE(use->work(0).known());
    }
    EXPECT_FALSE(use->work(0).known());
    std::thread untimed([&] { EXPECT_TRUE(use->work(0).isAssumed()); });
    untimed.join();
  }
  EXPECT_TRUE(use->work(0).isAssumed());
}

namespace {
class FilterCacheTimingTest : public ::testing::Test {
protected:
  test::CollectionHelper helper{"cache_timing_views"};
  std::shared_ptr<FilterCache> cache;
  std::shared_ptr<IndexReader> reader;
  std::shared_ptr<IndexReader> child;

  void SetUp() override {
    helper.clear();
    auto config = testConfig();
    config.admissionThreshold = 1;
    cache = std::make_shared<FilterCache>(config);
    auto writer = helper.getIndexWriter();
    writer->snapshots.readers.filterCache = cache;
    writer->mergePolicy->setMergeFactor(100);
    for (int batch = 0; batch < 2; batch++) {
      test::CollectionHelper::UpdateBuilder input;
      for (int i = 0; i < 16; i++) {
        auto json = "{\"id\":\"" + std::to_string(batch * 16 + i)
            + "\",\"rows_nested\":[{\"value_i\":0},{\"value_i\":1}]}";
        input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
      }
      input.commit();
      ASSERT_TRUE(helper.submit(input).success);
    }
    reader = writer->snapshots.readers.getReader();
    child = reader->view("rows_nested");
    ASSERT_EQ(2u, reader->segments().size());
    ASSERT_NE(reader->segments()[0].maxDoc(), child->segments()[0].maxDoc());
  }

  FilterCache::ValueDump value(const FilterKey& key, size_t ordinal = 0) {
    for (const auto& row : cache->dump(100, nullptr, true)) {
      if (row.keyHash != key.hash()) continue;
      for (const auto& value : row.values) {
        if (value.segId == reader->segments()[ordinal].segInfo.seg_id) return value;
      }
    }
    ADD_FAILURE() << "missing ledger";
    return {};
  }
};
}

TEST_F(FilterCacheTimingTest, alignedViewsShareTotalsAndSettleIndependently) {
  FilterCache::UseRegistry root(cache.get(), *reader);
  FilterCache::UseRegistry rows(cache.get(), *child, root);
  FilterCache::UseRegistry separate(cache.get(), *child);
  FilterKey rootKey("timed-owner"), childKey("timed-child"), untouched("untouched-child");
  auto* owner = root.get(rootKey);
  auto* nested = rows.get(childKey);
  auto* independent = separate.get(FilterKey("independent"));
  owner->observe(); nested->observe(); independent->observe();
  rows.get(untouched);
  {
    FilterCache::UseRegistry::SegmentScope pass(root, 0);
    EXPECT_FALSE(owner->work(0).known());
    EXPECT_FALSE(nested->work(0).known());
    EXPECT_TRUE(independent->work(0).isAssumed());
    EXPECT_TRUE(nested->work(1).isAssumed());
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, owner->probe(0, owner->work(0)).kind());
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, nested->probe(0, nested->work(0)).kind());
    root.addSegmentNanos(0, 400);
    rows.addSegmentNanos(0, 600);
  }
  root.settle(true);
  EXPECT_EQ(1u, value(rootKey).absentCount);
  EXPECT_EQ(0u, value(childKey).absentCount);
  rows.settle(true);
  EXPECT_EQ(1u, value(childKey).absentCount);
  EXPECT_DOUBLE_EQ(value(rootKey).absentNanos, value(childKey).absentNanos);
  EXPECT_DOUBLE_EQ(value(rootKey).owed, value(childKey).owed);
  EXPECT_EQ(0u, value(untouched).absentCount);
  EXPECT_DOUBLE_EQ(0, value(untouched).owed);
  auto counters = cache->counters();
  root.settle(true); rows.settle(true);
  EXPECT_EQ(counters, cache->counters());
}

TEST_F(FilterCacheTimingTest, nestedViewScopesDoNotRecordAnInnerInterval) {
  FilterCache::UseRegistry root(cache.get(), *reader);
  FilterCache::UseRegistry rows(cache.get(), *child, root);
  FilterKey key("nested-timing");
  auto* use = rows.get(key);
  use->observe();
  {
    FilterCache::UseRegistry::SegmentScope outer(root, 0);
    {
      FilterCache::UseRegistry::SegmentScope inner(rows, 0);
      use->probe(0, use->work(0));
      rows.addSegmentNanos(0, 1000);
    }
    // The outer scope has not recorded its interval yet.
    rows.settle(true);
    EXPECT_DOUBLE_EQ(1000, value(key).absentNanos);
    EXPECT_FALSE(use->work(0).known());
  }
  EXPECT_TRUE(use->work(0).isAssumed());
}

TEST_F(FilterCacheTimingTest, relatedViewsRequireTheSameSnapshotAndPhysicalSegments) {
  FilterCache::UseRegistry root(cache.get(), *reader);
  EXPECT_NO_THROW(FilterCache::UseRegistry(cache.get(), *child, root));
  ASSERT_TRUE(helper.deleteById("0", UpdateMessage::COMMIT).success);
  auto deleted = helper.getIndexWriter()->snapshots.readers.getReader();
  EXPECT_EQ(reader->segments()[0].segInfo.seg_id, deleted->segments()[0].segInfo.seg_id);
  EXPECT_THROW(FilterCache::UseRegistry(cache.get(), *deleted, root), std::invalid_argument);
  ASSERT_TRUE(helper.index(test::flatdoc("id", "new"), UpdateMessage::COMMIT).success);
  auto added = helper.getIndexWriter()->snapshots.readers.getReader();
  EXPECT_NE(reader->segments().size(), added->segments().size());
  EXPECT_THROW(FilterCache::UseRegistry(cache.get(), *added, root), std::invalid_argument);
}

TEST_F(FilterCacheTimingTest, claimedProductionCensorsOtherViewsAndAmbiguousOutcomes) {
  using Work = FilterCache::Work;
  for (bool rejected : {false, true}) for (bool priced : {false, true}) {
    SCOPED_TRACE(::testing::Message() << rejected << '/' << priced);
    cache->clear();
    FilterKey served("served-child"), known("known-owner"), ambiguous("ambiguous-owner");
    {
      FilterCache::UseRegistry seed(cache.get(), *child);
      auto* use = seed.get(served);
      auto build = use->probe(0);
      use->publishRaw(0, build, docs(child->segments()[0].maxDoc(), {7}), 1000);
      FilterCache::UseRegistry other(cache.get(), *reader);
      use = other.get(known);
      build = use->probe(0);
      use->publishRaw(0, build, docs(reader->segments()[0].maxDoc(), {7}), 1000);
    }
    FilterCache::UseRegistry root(cache.get(), *reader);
    FilterCache::UseRegistry rows(cache.get(), *child, root);
    FilterKey rootKey("censored-owner"), childKey("censored-child");
    root.get(rootKey)->probe(0, Work::pending());
    auto* nested = rows.get(childKey);
    nested->probe(0, Work::pending());
    nested->probe(1, Work::pending());
    rows.get(served)->probe(0, Work::pending());
    root.get(known)->probe(0, Work::nanoseconds(250));
    root.get(known)->probe(0, Work::pending());
    auto* both = root.get(ambiguous);
    both->probe(0, Work::pending());
    FilterCache::UseRegistry offering(cache.get(), *reader);
    offering.get(ambiguous)->offerRaw(0, docs(reader->segments()[0].maxDoc(), {7}), 1000);
    both->probe(0, Work::pending());
    auto* builder = root.get(FilterKey("production"));
    if (priced) {
      builder->probe(0, Work::pending());
      builder->charge(0, Work::nanoseconds(10'000));
    }
    {
      FilterCache::UseRegistry::SegmentScope pass(root, 0);
      auto probe = builder->probe(0, priced ? Work::pending() : Work::assumed());
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
      DocSetBuilder docs(reader->segments()[0].maxDoc());
      if (rejected) {
        for (int i = 0; i < reader->segments()[0].maxDoc(); i++) docs.add(i);
      } else {
        docs.add(7);
      }
      builder->publishRaw(0, probe, docs.build(), 5000);
    }
    root.addSegmentNanos(0, 9000);
    rows.addSegmentNanos(1, 600);
    auto before = cache->counters();
    root.settle(true); rows.settle(true);
    EXPECT_EQ(0u, value(rootKey).absentCount);
    EXPECT_EQ(0u, value(childKey).absentCount);
    EXPECT_EQ(0u, value(served).servedCount);
    EXPECT_EQ(0u, value(ambiguous).absentCount);
    EXPECT_EQ(0u, value(ambiguous).servedCount);
    EXPECT_DOUBLE_EQ(0, value(ambiguous).creditedTotal);
    EXPECT_DOUBLE_EQ(750, value(known).debt);
    EXPECT_DOUBLE_EQ(600, value(childKey, 1).owed);
    EXPECT_EQ(1u, value(childKey, 1).absentCount);
    auto after = cache->counters();
    EXPECT_EQ(before.observedCharges + 1, after.observedCharges);
    EXPECT_EQ(before.assumedCharges, after.assumedCharges);
    EXPECT_EQ(before.assumedCredits, after.assumedCredits);
    EXPECT_EQ(before.observedCredits, after.observedCredits);
    EXPECT_EQ(rejected, value(FilterKey("production")).resident == false);
  }
}

TEST_F(FilterCacheTimingTest, byproductsDoNotCensorAndAbortedViewsSettleNothing) {
  using Work = FilterCache::Work;
  for (bool completed : {false, true}) {
    FilterCache::UseRegistry root(cache.get(), *reader);
    FilterCache::UseRegistry rows(cache.get(), *child, root);
    FilterKey key(completed ? "byproduct-observation" : "aborted-observation");
    rows.get(key)->probe(0, Work::pending());
    {
      FilterCache::UseRegistry::SegmentScope pass(root, 0);
      root.get(FilterKey("offered"))->offerRaw(0, docs(reader->segments()[0].maxDoc(), {7}), 1000);
    }
    rows.addSegmentNanos(0, 1000);
    root.settle(completed); rows.settle(completed);
    EXPECT_EQ(completed ? 1u : 0u, value(key).absentCount);
  }
}

TEST_F(FilterCacheTimingTest, readerProductionCensorsOnlyItsExecutingTimedSegment) {
  using Work = FilterCache::Work;
  for (bool timed : {false, true}) {
    FilterCache::UseRegistry root(cache.get(), *reader);
    FilterCache::UseRegistry rows(cache.get(), *child, root);
    FilterKey key(timed ? "timed-reader-production" : "untimed-reader-production");
    rows.get(key)->probe(0, Work::pending());
    rows.get(key)->probe(1, Work::pending());
    auto* builder = root.get(FilterKey(timed ? "reader-timed" : "reader-untimed"), FilterKeyScope::READER_STABLE);
    auto domains = canonicalDomains(*reader);
    auto claim = builder->probeReaderStable(*reader, domains);
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, claim.kind());
    auto publish = [&] { builder->publishReaderStable(claim, oneDocPerSegment(*reader), 1000); };
    if (timed) {
      FilterCache::UseRegistry::SegmentScope pass(rows, 0);
      publish();
    } else {
      publish();
    }
    rows.addSegmentNanos(0, 1000);
    rows.addSegmentNanos(1, 1000);
    root.settle(true); rows.settle(true);
    EXPECT_EQ(timed ? 0u : 1u, value(key).absentCount);
    EXPECT_EQ(1u, value(key, 1).absentCount);
  }
}

TEST(FilterCacheTest, childExperimentLearnsZeroOrProfitableSaving) {
  auto config = testConfig(); config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  observedRequest(cache, segments, FilterKey("prior"), 1);
  observedRequest(cache, segments, FilterKey("prior"), 1, 90'000'000);
  FilterKey child("child-experiment");
  for (int i = 0; i < 29; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, segments, child, 3'200'000));
  }
  EXPECT_DOUBLE_EQ(92'800'000, ledgerFor(cache, child).owed);
  observedRequest(cache, segments, child, 1, 90'000'000);
  for (int i = 0; i < 5; i++) observedRequest(cache, segments, child, 3'200'000);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, child).creditedTotal);
  EXPECT_DOUBLE_EQ(90'000'000, ledgerFor(cache, child).debt);

  FilterKey profitable("profitable-child");
  for (int i = 0; i < 4; i++) observedRequest(cache, segments, profitable, 23'200'000);
  observedRequest(cache, segments, profitable, 1, 90'000'000);
  for (int i = 0; i < 5; i++) observedRequest(cache, segments, profitable, 3'200'000);
  EXPECT_DOUBLE_EQ(100'000'000, ledgerFor(cache, profitable).creditedTotal);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, profitable).debt);
  evictValues(cache);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, segments, child, 3'200'000));
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, observedRequest(cache, segments, profitable, 23'200'000));
  }
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, child).owed);
  EXPECT_DOUBLE_EQ(100'000'000, ledgerFor(cache, profitable).owed);
  observedRequest(cache, segments, profitable, 1, 90'000'000);
}

TEST(FilterCacheTest, unservedChildKeepsDebtAndFacesCapacityComparison) {
  size_t bytes = bitDocs(100, 7)->ramBytesUsed();
  auto config = capacityConfig(bytes + 1);
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey child("unserved-child");
  seedCapacityValue(cache, segments, child, 0, 90'000'000, true);
  evictValues(cache);
  observedRequest(cache, segments, child, 3'200'000);
  EXPECT_DOUBLE_EQ(90'000'000, ledgerFor(cache, child).debt);
  EXPECT_DOUBLE_EQ(3'200'000, ledgerFor(cache, child).owed);
  FilterKey resident("valuable-resident");
  seedCapacityValue(cache, segments, resident, 900'000'000, 90'000'000);
  fundCapacityValue(cache, segments, child, 180'000'000);
  auto before = cache.counters();
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(child);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
  EXPECT_EQ(before.capacityDeferred + 1, cache.counters().capacityDeferred);
  EXPECT_EQ(before.buildAttempts, cache.counters().buildAttempts);
}

TEST_F(FilterCacheTimingTest, ownerFundsBeforeChildAndProductionDoesNotFinishChildFunding) {
  using Work = FilterCache::Work;
  FilterKey owner("funded-owner"), childKey("nearly-funded-child");
  auto seed = [&](IndexReader& view, const FilterKey& key, uint64_t nanos) {
    FilterCache::UseRegistry request(cache.get(), view);
    auto* use = request.get(key);
    auto claim = use->probe(0);
    use->publishRaw(0, claim, docs(view.segments()[0].maxDoc(), {7}), nanos);
    FilterCache::UseRegistry serve(cache.get(), view);
    serve.get(key)->probe(0);
  };
  seed(*reader, owner, 85'000'000); seed(*child, childKey, 90'000'000);
  evictValues(*cache);
  for (int i = 0; i < 17; i++) {
    FilterCache::UseRegistry root(cache.get(), *reader), rows(cache.get(), *child, root);
    root.get(owner)->probe(0, Work::pending()); rows.get(childKey)->probe(0, Work::pending());
    root.addSegmentNanos(0, 5'000'000);
    root.settle(true); rows.settle(true);
  }
  FilterCache::UseRegistry root(cache.get(), *reader), rows(cache.get(), *child, root);
  {
    FilterCache::UseRegistry::SegmentScope pass(root, 0);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, rows.get(childKey)->probe(0, Work::pending()).kind());
    auto* use = root.get(owner);
    auto build = use->probe(0, Work::pending());
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
    use->publishRaw(0, build, docs(reader->segments()[0].maxDoc(), {7}), 85'000'000);
  }
  root.addSegmentNanos(0, 5'000'000);
  root.settle(true); rows.settle(true);
  EXPECT_DOUBLE_EQ(85'000'000, value(childKey).owed);
  EXPECT_EQ(17u, value(childKey).absentCount);
}

TEST_F(FilterCacheTimingTest, unknownPriorLetsTheFirstProbedChildBootstrap) {
  using Work = FilterCache::Work;
  FilterKey owner("bootstrap-owner"), childKey("bootstrap-child");
  {
    FilterCache::UseRegistry root(cache.get(), *reader), rows(cache.get(), *child, root);
    root.get(owner)->probe(0, Work::pending()); rows.get(childKey)->probe(0, Work::pending());
    root.addSegmentNanos(0, 5'000'000);
    root.settle(true); rows.settle(true);
  }
  FilterCache::UseRegistry root(cache.get(), *reader), rows(cache.get(), *child, root);
  {
    FilterCache::UseRegistry::SegmentScope pass(root, 0);
    auto* use = rows.get(childKey);
    auto build = use->probe(0, Work::pending());
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
    use->publishRaw(0, build, docs(child->segments()[0].maxDoc(), {7}), 90'000'000);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, root.get(owner)->probe(0, Work::pending()).kind());
  }
  root.settle(true); rows.settle(true);
  EXPECT_DOUBLE_EQ(5'000'000, value(owner).owed);
  EXPECT_DOUBLE_EQ(90'000'000, value(childKey).debt);
}

TEST(FilterCacheTest, untimedObservedWorkCannotChargeOrClaimEvenWhenFunded) {
  auto config = testConfig(); config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("untimed-child-or-source");
  for (int i = 0; i < 40; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key); use->observe();
    auto work = use->work(0).observedOnly();
    EXPECT_TRUE(work.isUnfunded());
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, work).kind());
    use->charge(0, work);
    EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).owed);
  }
  EXPECT_EQ(0u, cache.counters().assumedCharges);
  EXPECT_EQ(0u, cache.counters().buildAttempts);
  FilterCache::UseRegistry funded(cache, 1, segments);
  auto* use = funded.get(key);
  use->probe(0, FilterCache::Work::pending());
  use->charge(0, FilterCache::Work::nanoseconds(1000));
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, use->work(0).observedOnly()).kind());
  auto build = use->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
  use->publishRaw(0, build, docs(100, {7}), 1000);
  FilterCache::UseRegistry serve(cache, 1, segments);
  auto* hit = serve.get(key);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, hit->probe(0, hit->work(0).observedOnly()).kind());
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).debt);
}

TEST(FilterCacheTest, zeroCreditServeLeavesAnotherConsumersPaymentAvailable) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("count-without-payload-credit");
  servePolicyValue(cache, segments, key, 7, 1000);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto candidate = request.lookupExisting(key);
  ASSERT_TRUE(candidate);
  auto before = cache.counters();
  auto* use = request.acceptExisting(std::move(*candidate),
      FilterCache::AdmissionLane::WHOLE, FilterCache::Work::nanoseconds(0));
  ASSERT_NE(nullptr, use);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0, FilterCache::Work::nanoseconds(0)).kind());
  EXPECT_EQ(before.hits + 1, cache.counters().hits);
  EXPECT_EQ(before.assumedCredits, cache.counters().assumedCredits);
  EXPECT_EQ(before.observedCredits, cache.counters().observedCredits);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).creditedTotal);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  evictValues(cache);
  EXPECT_EQ(before.unrepaidEvictions + 1, cache.counters().unrepaidEvictions);
  EXPECT_EQ(before.unpaidBuildNanos + 1000, cache.counters().unpaidBuildNanos);
  EXPECT_EQ(use, request.get(key));
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0).kind());
  use->credit(0, FilterCache::Work::assumed());
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).debt);
  EXPECT_EQ(before.assumedCredits + 1, cache.counters().assumedCredits);
  EXPECT_EQ(before.estimatedSavedNanos + 1000, cache.counters().estimatedSavedNanos);
}

TEST(FilterCacheTest, zeroCreditValueLosesToRealSavedWork) {
  constexpr int32_t maxDoc = 1024;
  size_t bytes = bitDocs(maxDoc, 7)->ramBytesUsed();
  FilterCache cache(capacityConfig(2 * bytes, 8));
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  cache.onReaderPublished(1, segments);
  FilterKey count("count-value"), real("real-saving");
  seedCapacityValue(cache, segments, count, 0);
  seedCapacityValue(cache, segments, real, 0);
  for (int i = 0; i < 2; i++) {
    advanceCacheActivity(cache, segments, 8);
    cache.sweep();
  }
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::HIT,
        request.get(real)->probe(0, FilterCache::Work::nanoseconds(1000)).kind());
  }
  advanceCacheActivity(cache, segments, 8);
  cache.sweep();
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::HIT,
        request.get(count)->probe(0, FilterCache::Work::nanoseconds(0)).kind());
    request.get(FilterKey("new-value"))->offerRaw(0, bitDocs(maxDoc, 7), 1000);
  }
  EXPECT_FALSE(ledgerFor(cache, count).resident);
  EXPECT_TRUE(ledgerFor(cache, real).resident);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, count).debt);
  EXPECT_EQ(1u, cache.counters().unrepaidEvictions);
}

TEST(FilterCacheTest, completeResidencyAcceptanceCanDeferObservedCredit) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("complete-observed-credit");
  servePolicyValue(cache, segments, key, 7, 1000);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto before = cache.counters();
  auto candidate = request.lookupExisting(key);
  ASSERT_TRUE(candidate);
  EXPECT_EQ(before, cache.counters());
  auto* use = request.acceptExisting(std::move(*candidate),
      FilterCache::AdmissionLane::WHOLE, FilterCache::Work::pending());
  ASSERT_NE(nullptr, use);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  use->credit(0, FilterCache::Work::nanoseconds(250));
  use->probe(0, FilterCache::Work::nanoseconds(750));
  EXPECT_DOUBLE_EQ(750, ledgerFor(cache, key).debt);
  EXPECT_EQ(1u, cache.counters().hits);
  EXPECT_EQ(1u, cache.counters().observedCredits);
  EXPECT_EQ(250u, cache.counters().estimatedSavedNanos);
}

TEST(FilterCacheTest, partialCreditsCarryInvestmentAcrossEvictionAndRebuild) {
  auto config = testConfig();
  config.admissionThreshold = 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("partial-credit");
  using Work = FilterCache::Work;
  servePolicyValue(cache, segments, key, 7, 1000);
  for (double nanos : {40, 60}) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto candidate = use->look(0);
    ASSERT_TRUE(candidate);
    auto hit = use->accept(std::move(*candidate), Work::pending());
    use->credit(0, Work::nanoseconds(nanos));
    use->credit(0, Work::nanoseconds(nanos));
  }
  EXPECT_NEAR(100, ledgerFor(cache, key).creditedTotal, 1e-15);
  evictValues(cache);
  EXPECT_EQ(1u, cache.counters().unrepaidEvictions);
  EXPECT_EQ(900u, cache.counters().unpaidBuildNanos);
  EXPECT_NEAR(900, ledgerFor(cache, key).debt, 1e-15);
  auto charge = [&](double nanos) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, Work::pending()).kind());
    use->charge(0, Work::nanoseconds(nanos));
  };
  auto rebuild = [&] {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto build = use->probe(0, Work::pending());
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
    use->publishRaw(0, build, bitDocs(4096, 7), 1000);
  };
  charge(1800);
  charge(100);
  rebuild();
  EXPECT_NEAR(1900, ledgerFor(cache, key).debt, 1e-15);
  evictValues(cache);
  charge(2800);
  charge(100);
  rebuild();
  EXPECT_NEAR(2900, ledgerFor(cache, key).debt, 1e-15);
  FilterCache::UseRegistry hitRequest(cache, 1, segments);
  auto* use = hitRequest.get(key);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, use->probe(0, Work::nanoseconds(2900)).kind());
  EXPECT_NEAR(0, ledgerFor(cache, key).debt, 1e-15);
  cache.validateForTest();
}

TEST(FilterCacheTest, lateCreditPaysOnceWithoutForgivingNewInvestment) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("late-ledger-credit");
  using Work = FilterCache::Work;
  servePolicyValue(cache, segments, key, 7, 1000);
  FilterCache::UseRegistry oldRequest(cache, 1, segments);
  auto* old = oldRequest.get(key);
  auto candidate = old->look(0);
  ASSERT_TRUE(candidate);
  evictValues(cache);
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, servePolicyValue(cache, segments, key, 8, 2000));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, servePolicyValue(cache, segments, key, 8, 2000));
  EXPECT_DOUBLE_EQ(3000, ledgerFor(cache, key).debt);
  EXPECT_TRUE(old->accept(std::move(*candidate), Work::nanoseconds(1000)).docSet()->get(7));
  old->credit(0, Work::nanoseconds(1000));
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, key).debt);
  EXPECT_EQ(1000u, cache.counters().estimatedSavedNanos);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).creditedTotal);
  FilterCache::UseRegistry current(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, current.get(key)->probe(0).kind());
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).debt);
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, key).creditedTotal);
}

TEST(FilterCacheTest, assumedCreditRepaysOneInvestmentAndCounterResetKeepsDebt) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("assumed-one-unit");
  servePolicyValue(cache, segments, key, 7, 1000);
  evictValues(cache);
  servePolicyValue(cache, segments, key, 7, 1000);
  servePolicyValue(cache, segments, key, 7, 1000);
  EXPECT_DOUBLE_EQ(2000, ledgerFor(cache, key).debt);
  servePolicyValue(cache, segments, key, 7, 1000);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  EXPECT_EQ(1000u, cache.counters().outstandingDebtNanos);
  cache.resetCounters();
  EXPECT_EQ(1000u, cache.counters().outstandingDebtNanos);
  EXPECT_EQ(0u, cache.counters().assumedCredits);
  cache.clear();
  EXPECT_EQ(0u, cache.counters().outstandingDebtNanos);
  EXPECT_EQ(0u, cache.entryCountForTest());
}

TEST(FilterCacheTest, rejectedInvestmentAddsDebtAndByproductsDoNot) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("rejected-investment");
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto build = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
    use->publishRaw(0, build, DocSet::andNot(nullptr, {}, 100), 1000);
  }
  EXPECT_EQ(1u, cache.counters().publishRejects);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
              use->probe(0, FilterCache::Work::nanoseconds(500)).kind());
  }
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(key)->offerRaw(0, docs(100, {7}), 2000);
  }
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  EXPECT_DOUBLE_EQ(500, ledgerFor(cache, key).owed);
  EXPECT_EQ(1000u, ledgerFor(cache, key).productionNanos);
  EXPECT_GT(ledgerFor(cache, key).referenceBytes, 0u);
  FilterKey byproduct("claimed-byproduct");
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(byproduct);
  auto build = use->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
  use->publishRawByproduct(0, build, docs(100, {7}), 1000);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, byproduct).debt);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, byproduct).owed);
  cache.validateForTest();
}

TEST(FilterCacheTest, concurrentGrantsSpendOwedOnlyOnCompletion) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key("ledger-claimants");
  using Work = FilterCache::Work;
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    use->probe(0, Work::pending());
    use->charge(0, Work::nanoseconds(1));
  }
  std::barrier phase(2);
  std::atomic<int> grants{0};
  auto claimant = [&] {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    phase.arrive_and_wait();
    auto probe = use->probe(0, Work::pending());
    if (probe.kind() == FilterCache::Probe::Kind::BUILD) grants.fetch_add(1);
    phase.arrive_and_wait();
    EXPECT_DOUBLE_EQ(1, ledgerFor(cache, key).owed);
    phase.arrive_and_wait();
  };
  std::thread first(claimant), second(claimant);
  first.join(); second.join();
  EXPECT_EQ(1, grants.load());
  EXPECT_DOUBLE_EQ(1, ledgerFor(cache, key).owed);
  EXPECT_THROW({
    FilterCache::UseRegistry request(cache, 1, segments);
    auto probe = request.get(key)->probe(0, Work::pending());
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    auto moved = std::move(probe);
    throw std::runtime_error("production failed");
  }, std::runtime_error);
  EXPECT_DOUBLE_EQ(1, ledgerFor(cache, key).owed);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).debt);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  auto build = use->probe(0, Work::pending());
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
  {
    FilterCache::UseRegistry duringBuild(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
              duringBuild.get(key)->probe(0, Work::nanoseconds(0.4)).kind());
  }
  EXPECT_DOUBLE_EQ(1.4, ledgerFor(cache, key).owed);
  use->publishRaw(0, build, docs(100, {7}), 1000);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).owed);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  cache.validateForTest();
}

TEST(FilterCacheTest, readerNanosecondDebtSurvivesEvictionAndRebuild) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir));
  IndexWriter writer(snapshots);
  addTermDoc(writer, "first");
  addTermDoc(writer, "second");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  auto config = testConfig();
  config.admissionThreshold = 1;

  FilterCache cache(config);
  cache.onReaderPublished(*reader);
  FilterKey key("reader-nanosecond-debt");
  using Work = FilterCache::Work;
  auto build = [&](Work work) {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    auto probe = use->probeReaderStable(*reader, domains, work);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
    use->publishReaderStable(probe, oneDocPerSegment(*reader), 1000);
  };
  auto charge = [&](double nanos) {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
              use->probeReaderStable(*reader, domains, Work::pending()).kind());
    use->chargeReaderStable(Work::nanoseconds(nanos));
    use->chargeReaderStable(Work::nanoseconds(nanos));
  };
  build(Work::assumed());
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto candidate = request.lookupExisting(key, FilterKeyScope::READER_STABLE);
    ASSERT_TRUE(candidate);
    auto* use = request.acceptExisting(std::move(*candidate),
        FilterCache::AdmissionLane::WHOLE, Work::pending());
    ASSERT_NE(nullptr, use);
    use->creditReaderStable(Work::nanoseconds(100));
    use->creditReaderStable(Work::nanoseconds(100));
  }
  evictValues(cache);
  EXPECT_NEAR(900, ledgerFor(cache, key).debt, 1e-15);
  EXPECT_EQ(900u, cache.counters().unpaidBuildNanos);
  charge(1800); charge(100);
  build(Work::pending());
  EXPECT_NEAR(1900, ledgerFor(cache, key).debt, 1e-15);
  evictValues(cache);
  charge(2800); charge(100);
  build(Work::pending());
  EXPECT_NEAR(2900, ledgerFor(cache, key).debt, 1e-15);
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::HIT,
              use->probeReaderStable(*reader, domains, Work::nanoseconds(2900)).kind());
  }
  EXPECT_NEAR(0, ledgerFor(cache, key).debt, 1e-15);
  cache.validateForTest();
}

TEST(FilterCacheTest, readerFailedProductionLeavesOwedAvailable) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir));
  IndexWriter writer(snapshots);
  addTermDoc(writer, "first");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  cache.onReaderPublished(*reader);
  FilterKey key("reader-failed-production");
  using Work = FilterCache::Work;
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD,
              use->probeReaderStable(*reader, domains, Work::nanoseconds(1)).kind());
  }
  EXPECT_THROW({
    FilterCache::UseRegistry request(cache, *reader);
    auto probe = request.get(key, FilterKeyScope::READER_STABLE)
        ->probeReaderStable(*reader, domains, Work::pending());
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
    FilterCache::ReaderProbe moved;
    moved = std::move(probe);
    auto again = std::move(moved);
    throw std::runtime_error("production failed");
  }, std::runtime_error);
  EXPECT_DOUBLE_EQ(1, ledgerFor(cache, key).owed);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).debt);
  FilterCache::UseRegistry request(cache, *reader);
  auto* use = request.get(key, FilterKeyScope::READER_STABLE);
  auto build = use->probeReaderStable(*reader, domains, Work::pending());
  ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, build.kind());
  use->publishReaderStable(build, oneDocPerSegment(*reader), 1000);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, key).owed);
  EXPECT_DOUBLE_EQ(1000, ledgerFor(cache, key).debt);
  cache.validateForTest();
}

TEST(FilterCacheTest, unrepaidEvictionsRequireTriangularWorkInOrderedReplay) {
  auto sample = bitDocs(4096, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 64;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 3 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  std::array hotKeys{FilterKey("hot-value-0"), FilterKey("hot-value-1")};
  constexpr int deadCount = 8;
  std::array<FilterKey, deadCount> deadKeys;
  std::array<int, deadCount> claimedBuilds{};
  std::array<bool, deadCount> sharedHits{};
  for (int i = 0; i < deadCount; i++) {
    deadKeys[i] = FilterKey("dead-value-" + std::to_string(i));
  }

  for (int i = 0; i < (int)hotKeys.size(); i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
              servePolicyValue(cache, segments, hotKeys[i], i + 1, 100'000));
    EXPECT_EQ(FilterCache::Probe::Kind::HIT,
              servePolicyValue(cache, segments, hotKeys[i], i + 1, 100'000));
  }

  advanceCacheActivity(cache, segments, 64);
  cache.sweep();
  constexpr int rounds = 64;
  for (int round = 0; round < rounds; round++) {
    for (int i = 0; i < (int)hotKeys.size(); i++) {
      EXPECT_EQ(FilterCache::Probe::Kind::HIT,
                servePolicyValue(
                    cache, segments, hotKeys[i], i + 1, 100'000));
    }
    for (int i = 0; i < deadCount; i++) {
      servePolicyValue(cache, segments, deadKeys[i], 10 + i, 1,
                       &claimedBuilds[i], &sharedHits[i]);
    }
    for (int i = 0; i < (int)hotKeys.size(); i++) {
      EXPECT_EQ(FilterCache::Probe::Kind::HIT,
                servePolicyValue(
                    cache, segments, hotKeys[i], i + 1, 100'000));
    }
  }

  // n unrepaid investments require 1 + 2 + ... + n ordinary uses.
  constexpr int linearDebtBuildBound = 10;  // 55 <= 64 < 66
  int totalDeadBuilds = 0;
  for (int i = 0; i < deadCount; i++) {
    EXPECT_LE(claimedBuilds[i],
              linearDebtBuildBound + (sharedHits[i] ? 1 : 0));
    totalDeadBuilds += claimedBuilds[i];
  }
  auto counters = cache.counters();
  EXPECT_GT(counters.populationDeferred, 0u);
  EXPECT_EQ(2u + (uint64_t)totalDeadBuilds, counters.buildAttempts);
  EXPECT_EQ((uint64_t)totalDeadBuilds, counters.unrepaidEvictions);
  EXPECT_LT(cache.bytesUsed(), charge * deadCount / 2);
  for (int i = 0; i < (int)hotKeys.size(); i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::HIT,
              servePolicyValue(cache, segments, hotKeys[i], i + 1, 100'000));
  }
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, untimedObservedMissDoesNotFundPopulation) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey victim("borrow-victim");
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, victim, 1, 1));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, FilterKey("borrow-winner"), 2, 100'000));
  ASSERT_EQ(1u, cache.counters().unrepaidEvictions);

  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(victim);
  ASSERT_TRUE(use->wasAdmitted());
  auto before = cache.counters();
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
            use->probe(0, use->work(0).observedOnly()).kind());
  use->charge(0, FilterCache::Work::nanoseconds(100));
  auto after = cache.counters();
  EXPECT_EQ(before.misses + 1, after.misses);
  EXPECT_EQ(before.buildAttempts, after.buildAttempts);
  EXPECT_EQ(before.populationDeferred, after.populationDeferred);
  EXPECT_EQ(before.observedCharges, after.observedCharges);
  EXPECT_DOUBLE_EQ(0, ledgerFor(cache, victim).owed);
  // An untimed observation leaves owed at zero. One assumed charge cannot fund
  // another build, and repeated probes in this request cannot add to it.
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0).kind());
  }
  EXPECT_EQ(before.populationDeferred + 1, cache.counters().populationDeferred);
  EXPECT_EQ(before.misses + 1, cache.counters().misses);
  cache.resize(charge * 2);
  FilterCache::UseRegistry next(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, next.get(victim)->probe(0).kind());
  EXPECT_EQ(before.buildAttempts + 1, cache.counters().buildAttempts);
  cache.validateForTest();
}

TEST(FilterCacheTest, untimedObservedServePinsAcrossEviction) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("borrowed-value");
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, key, 1, 1));
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  DocSet* borrowed;
  {
    auto hit = use->probe(0, use->work(0).observedOnly());
    ASSERT_EQ(FilterCache::Probe::Kind::HIT, hit.kind());
    borrowed = hit.docSet();
  }
  auto hits = cache.counters().hits;
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, FilterKey("replacement"), 2, 100'000));
  ASSERT_EQ(1u, cache.counters().evictions);
  auto hit = use->probe(0, use->work(0).observedOnly());
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, hit.kind());
  EXPECT_EQ(borrowed, hit.docSet());
  EXPECT_TRUE(borrowed->get(1));
  EXPECT_EQ(1, borrowed->card());
  EXPECT_EQ(hits, cache.counters().hits); // request-local pin
  cache.validateForTest();
}

TEST(FilterCacheTest, untimedAndFundedConsumersShareOneRequestValue) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterCache::UseRegistry request(cache, 1, segments);
  FilterKey key("same-request");
  auto* borrowing = request.get(key);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
            borrowing->probe(0, borrowing->work(0).observedOnly()).kind());
  auto* ordinary = request.get(key);
  EXPECT_EQ(borrowing, ordinary);
  auto build = ordinary->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
  ordinary->publishRaw(0, build, docs(100, {7}), 1);
  auto hit = borrowing->probe(0, borrowing->work(0).observedOnly());
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, hit.kind());
  EXPECT_TRUE(hit.docSet()->get(7));
  EXPECT_EQ(1u, cache.counters().buildAttempts);
  EXPECT_EQ(1u, cache.counters().builds);
  cache.validateForTest();
}

TEST(FilterCacheTest, evictionDebtRecoversThroughCredits) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("recovering");
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, servePolicyValue(cache, segments, key, 2, 1));
  evictValues(cache);
  EXPECT_EQ(1u, cache.counters().unrepaidEvictions);
  EXPECT_EQ(1u, cache.counters().unpaidBuildNanos);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, servePolicyValue(cache, segments, key, 2, 1));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, servePolicyValue(cache, segments, key, 2, 1));
  EXPECT_EQ(2, ledgerFor(cache, key).debt);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, servePolicyValue(cache, segments, key, 2, 1));
  EXPECT_EQ(1, ledgerFor(cache, key).debt);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, servePolicyValue(cache, segments, key, 2, 1));
  EXPECT_EQ(0, ledgerFor(cache, key).debt);
  evictValues(cache);
  EXPECT_EQ(1u, cache.counters().unrepaidEvictions);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, servePolicyValue(cache, segments, key, 2, 1));
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, byproductEvictionDoesNotAccrueDeadBuildDebt) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey byproduct("byproduct-victim");

  {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(byproduct)->offerRaw(0, bitDocs(4096, 1), 1);
  }
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(
                cache, segments, FilterKey("byproduct-winner"), 2, 100'000));
  EXPECT_EQ(0u, cache.counters().unrepaidEvictions);
  EXPECT_EQ(0u, cache.counters().unpaidBuildNanos);
  EXPECT_EQ(100'000u, cache.counters().buildNanos);
  EXPECT_EQ(0u, cache.counters().populationDeferred);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, byproduct, 1, 1));
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, purgeAndMetadataSweepDiscardLedgerGhosts) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  config.maxMetadataEntries = 2;
  FilterCache cache(config);
  std::array oldSegments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, oldSegments));
  FilterKey winner("ghost-winner");
  FilterKey victim("ghost-victim");
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, oldSegments, winner, 1, 100'000));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, oldSegments, victim, 2, 1));
  ASSERT_EQ(1u, cache.counters().unrepaidEvictions);
  const void* oldEntry = cache.entryIdentityForTest(victim);
  ASSERT_NE(nullptr, oldEntry);

  std::array newSegments{FilterCache::SegmentIdentity{2, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(2, newSegments));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, newSegments, winner, 1, 100'000,
                             nullptr, nullptr, 2));
  {
    FilterCache::UseRegistry metadata(cache, 2, newSegments);
    metadata.get(FilterKey("ghost-metadata-sweep"));
  }
  EXPECT_EQ(nullptr, cache.entryIdentityForTest(victim));

  uint64_t skipsBefore = cache.counters().populationDeferred;
  FilterCache::UseRegistry retry(cache, 2, newSegments);
  auto* retryUse = retry.get(victim);
  EXPECT_NE(nullptr, retryUse->entryIdentityForTest());
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, retryUse->probe(0).kind());
  EXPECT_EQ(skipsBefore, cache.counters().populationDeferred);
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, recentWorkIncludesEffectiveResidencyForTinyPayloads) {
  auto tiny = docs(4096, {1});
  auto large = bitDocs(4096, 2);
  size_t tinyCharge = tiny->ramBytesUsed();
  size_t largeCharge = large->ramBytesUsed();
  constexpr uint32_t TINY_COST = 100;
  constexpr uint32_t LARGE_COST = 1000;
  // Payload-only work density ranks tiny first, which is the pathology this test
  // excludes once fixed/value and per-entry residency are included.
  ASSERT_GT((double) TINY_COST / (double) tinyCharge,
            (double) LARGE_COST / (double) largeCharge);

  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 16;
  config.maxEntryBytes = largeCharge + 1;
  config.maxBytes = tinyCharge + largeCharge - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey tinyKey("tiny-density");
  FilterKey largeKey("wide-density");
  ASSERT_EQ(tinyKey.bytes().capacity(), largeKey.bytes().capacity());

  FilterCache::UseRegistry request(cache, 1, segments);
  auto* tinyUse = request.get(tinyKey);
  auto tinyProbe = tinyUse->probe(0, FilterCache::Work::nanoseconds(TINY_COST));
  tinyUse->publishRaw(0, tinyProbe, std::move(tiny), TINY_COST);
  auto* largeUse = request.get(largeKey);
  auto largeProbe = largeUse->probe(0, FilterCache::Work::nanoseconds(LARGE_COST));
  largeUse->publishRaw(0, largeProbe, std::move(large), LARGE_COST);

  EXPECT_EQ(largeCharge, cache.bytesUsed());
  EXPECT_EQ(1u, cache.counters().evictions);
  FilterCache::UseRegistry verify(cache, 1, segments);
  EXPECT_NE(FilterCache::Probe::Kind::HIT,
            verify.get(tinyKey)->probe(0).kind());
  EXPECT_EQ(FilterCache::Probe::Kind::HIT,
            verify.get(largeKey)->probe(0).kind());
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, publicationAgeBreaksZeroWorkTies) {
  auto sample = bitDocs(1024, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey expensive("cold-expensive");

  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(expensive);
    auto probe = use->probe(0, FilterCache::Work::nanoseconds(0).forWholeQuery());
    use->publishRaw(0, probe, std::move(sample), 100);
  }

  for (int i = 0; i < 12; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(
        FilterKey("cheap-" + std::to_string(i)));
    auto probe = use->probe(0, FilterCache::Work::nanoseconds(0).forWholeQuery());
    use->publishRaw(0, probe, bitDocs(1024, i + 2), 9);
  }

  EXPECT_EQ(charge, cache.bytesUsed());
  EXPECT_EQ(12u, cache.counters().evictions);
  FilterCache::UseRegistry verify(cache, 1, segments);
  EXPECT_NE(FilterCache::Probe::Kind::HIT,
            verify.get(expensive)->probe(0).kind());
  EXPECT_EQ(FilterCache::Probe::Kind::HIT,
            verify.get(FilterKey("cheap-11"))->probe(0).kind());
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, purgeDetachesButRequestPinSurvives) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* cacheUse = request.get(FilterKey("pin"));
  auto probe = cacheUse->probe(0);
  auto pinned = cacheUse->publishRaw(0, probe, docs(100, {8}), 1);
  ASSERT_GT(cache.bytesUsed(), 0u);

  std::array replacement{FilterCache::SegmentIdentity{2, 100}};
  cache.onReaderPublished(2, replacement);
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_EQ(1u, cache.counters().purges);
  EXPECT_TRUE(pinned->docSet()->get(8));
}

TEST(FilterCacheTest, metadataEntriesHaveSeparateBound) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxMetadataEntries = 2;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  for (int i = 0; i < 6; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(FilterKey("metadata-" + std::to_string(i)));
  }
  cache.sweep();
  EXPECT_LE(cache.entryCountForTest(), 2u);
}

TEST(FilterCacheTest, metadataBytesAreAccountedAndSweepNonemptyEntries) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = 1024;
  config.maxMetadataEntries = 100;
  config.maxMetadataBytes = 1280;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey first(std::string(512, 'a'));
  FilterKey second(std::string(512, 'b'));

  FilterCache::UseRegistry firstRequest(cache, 1, segments);
  auto* firstUse = firstRequest.get(first);
  auto firstProbe = firstUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, firstProbe.kind());
  firstUse->publishRaw(0, firstProbe, docs(100, {1}), 1);
  EXPECT_GT(cache.metadataBytesUsed(), first.bytes().size());
  EXPECT_GT(cache.bytesUsed(), 0u);

  FilterCache::UseRegistry secondRequest(cache, 1, segments);
  auto* secondUse = secondRequest.get(second);
  EXPECT_TRUE(secondUse->wasAdmitted());

  EXPECT_EQ(1u, cache.entryCountForTest());
  EXPECT_LE(cache.metadataBytesUsed(), config.maxMetadataBytes);
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_EQ(1u, cache.counters().evictions);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, secondUse->probe(0).kind());
  FilterCache::UseRegistry verify(cache, 1, segments);
  EXPECT_NE(FilterCache::Probe::Kind::HIT, verify.get(first)->probe(0).kind());

  cache.clear();
  EXPECT_EQ(0u, cache.metadataBytesUsed());
}

// A long-lived entry used across many merges keeps one slot per live
// segment: merged-away slots are pruned on publication, and their metadata
// charge is released.
TEST(FilterCacheTest, mergedAwaySlotsArePruned) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  FilterKey key("tenant:42");
  size_t steadyMetadata = 0;
  // Aligned on the previous reader and not yet probed.
  std::unique_ptr<FilterCache::UseRegistry> held;

  for (uint64_t gen = 1; gen <= 50; gen++) {
    // Each publication merges away the oldest segment and adds a new one.
    std::array segments{FilterCache::SegmentIdentity{gen, 100},
                        FilterCache::SegmentIdentity{gen + 1, 100},
                        FilterCache::SegmentIdentity{gen + 2, 100}};
    ASSERT_TRUE(cache.onReaderPublished(gen, segments));
    EXPECT_LE(cache.slotCountForTest(key), segments.size());
    if (held != nullptr) {
      // The held Use still owns the pruned slot of the merged-away segment;
      // it stays a miss, while the surviving segment still hits.
      auto* stale = held->get(key);
      EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, stale->probe(0).kind());
      EXPECT_EQ(FilterCache::Probe::Kind::HIT, stale->probe(1).kind());
      held.reset();
    }

    FilterCache::UseRegistry request(cache, gen, segments);
    auto* use = request.get(key);
    ASSERT_TRUE(use->wasAdmitted());
    for (size_t ord = 0; ord < segments.size(); ord++) {
      auto probe = use->probe(ord);
      if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
        use->publishRaw(ord, probe, bitDocs(100, 7), 10);
      }
    }
    EXPECT_EQ(segments.size(), cache.slotCountForTest(key));

    // Every segment has a slot now, so this Use aligns without inserting.
    FilterCache::UseRegistry again(cache, gen, segments);
    auto* hot = again.get(key);
    for (size_t ord = 0; ord < segments.size(); ord++) {
      EXPECT_EQ(FilterCache::Probe::Kind::HIT, hot->probe(ord).kind());
    }
    EXPECT_EQ(segments.size(), cache.slotCountForTest(key));
    if (gen == 1) steadyMetadata = cache.metadataBytesUsed();
    EXPECT_EQ(steadyMetadata, cache.metadataBytesUsed());
    cache.validateForTest();

    held = std::make_unique<FilterCache::UseRegistry>(cache, gen, segments);
    held->get(key);
  }
  held.reset();

  cache.clear();
  EXPECT_EQ(0u, cache.metadataBytesUsed());
  EXPECT_EQ(0u, cache.bytesUsed());
  cache.validateForTest();

  test::CollectionHelper helper("merged_nested_cache");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  auto shared = std::make_shared<FilterCache>(FilterCacheConfig{
      .maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 2});
  writer->snapshots.readers.filterCache = shared;
  for (int batch = 0; batch < 2; batch++) {
    test::CollectionHelper::UpdateBuilder input;
    for (int i = 0; i < 8; i++) {
      auto json = "{\"id\":\"" + std::to_string(batch * 8 + i)
          + "\",\"items_nested\":{\"color_s\":\"" + (i % 2 ? "red" : "blue") + "\"}}";
      input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
    }
    input.commit(); ASSERT_TRUE(helper.submit(input).success);
  }
  auto count = [&](bool child) {
    auto req = test::localReq(helper.getSearchEngine());
    auto& op = req->collection("merged_nested_cache").topDocs().countLimit().limit(child ? 0 : 1);
    if (child) {
      op.allQuery().matchFilter("color_s", "red").rawOp().domain.emplace().to = "items_nested";
    } else {
      op.rawQuery() = test::qb::toParent(req->mr, "items_nested",
          test::qb::match(req->mr, "color_s", "red"), "none");
    }
    req->execute(1); EXPECT_OK(req); EXPECT_EQ(8, req->getMatchCount());
  };
  for (int i = 0; i < 3; i++) { count(false); count(true); }
  // Only owner membership is resident: to_parent does not populate its raw
  // child set, and the child-view term count resolves from metadata.
  auto entries = shared->dump(10, nullptr);
  ASSERT_EQ(1u, entries.size());
  for (const auto& entry : entries) EXPECT_EQ(2u, entry.segmentsResident);
  auto warm = shared->counters();
  EXPECT_GT(warm.hits, 0u);
  auto old = writer->snapshots.readers.getReader();
  ASSERT_EQ(2u, old->segments().size());
  test::CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1); ASSERT_TRUE(helper.submit(merge).success);
  auto merged = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, merged->segments().size());
  for (const auto& segment : old->segments()) {
    EXPECT_NE(segment.segInfo.seg_id, merged->segments()[0].segInfo.seg_id);
  }
  EXPECT_TRUE(shared->dump(10, nullptr).empty());
  EXPECT_EQ(warm.purges + 2, shared->counters().purges);
  EXPECT_EQ(0u, shared->bytesUsed());
  // Admission history belongs to the key; the first new sighting builds a
  // fresh slot under the merged segment identity, then both views hit it.
  count(false); count(true);
  auto rebuilt = shared->dump(10, nullptr);
  ASSERT_EQ(entries.size(), rebuilt.size());
  for (const auto& entry : rebuilt) {
    EXPECT_EQ(1u, entry.segmentsResident);
    EXPECT_TRUE(std::ranges::any_of(entries, [&](const auto& prior) { return prior.keyHash == entry.keyHash; }));
  }
  EXPECT_EQ(warm.builds + 1, shared->counters().builds);
  auto hits = shared->counters().hits;
  count(false); count(true);
  EXPECT_GT(shared->counters().hits, hits);
  shared->validateForTest();
}

// A metadata sweep started at the byte cap trims to the low watermark, so
// the admissions that follow do not each start another sweep.
TEST(FilterCacheTest, metadataSweepTrimsToLowWatermark) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxMetadataEntries = 1000;
  config.maxMetadataBytes = 16 * 1024;
  config.lowWatermarkMetadataBytes = 8 * 1024;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);

  size_t sweeps = 0;
  size_t previous = 0;
  for (int i = 0; i < 100; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    request.get(FilterKey("key:" + std::to_string(i) + std::string(200, 'k')));
    size_t used = cache.metadataBytesUsed();
    EXPECT_LE(used, config.maxMetadataBytes);
    if (used < previous) {
      sweeps++;
      EXPECT_LE(used, config.lowWatermarkMetadataBytes);
    }
    previous = used;
  }
  EXPECT_GE(sweeps, 2u);
  EXPECT_LE(sweeps, 15u);
  cache.validateForTest();
}

TEST(FilterCacheTest, oversizedKeysBypassWithoutAffectingAdmission) {
  FilterCacheConfig config = testConfig();
  config.maxEntryBytes = 32;
  config.admissionThreshold = 2;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  uint64_t sharedHash = 12345;
  FilterKey oversized = FilterKey::withHashForTest(std::string(33, 'x'),
                                                    sharedHash);

  for (int i = 0; i < 2; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(oversized);
    EXPECT_FALSE(use->wasAdmitted());
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0).kind());
  }
  EXPECT_EQ(0u, cache.entryCountForTest());
  EXPECT_EQ(0u, cache.metadataBytesUsed());
  EXPECT_EQ(2u, cache.counters().oversizedKeyBypasses);

  FilterKey small = FilterKey::withHashForTest("small", sharedHash);
  FilterCache::UseRegistry firstSmall(cache, 1, segments);
  EXPECT_FALSE(firstSmall.get(small)->wasAdmitted());
  FilterCache::UseRegistry secondSmall(cache, 1, segments);
  EXPECT_TRUE(secondSmall.get(small)->wasAdmitted());
}

TEST(FilterCacheTest, largeKeyBelowCapAdmitsAndHits) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = 20 * 1024;
  config.maxMetadataBytes = 32 * 1024;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterKey key(std::string(16 * 1024, 'k'));

  FilterCache::UseRegistry populate(cache, 1, segments);
  auto* use = populate.get(key);
  auto probe = use->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
  use->publishRaw(0, probe, docs(100, {3, 7}), 1);

  EXPECT_EQ(1u, cache.entryCountForTest());
  EXPECT_GT(cache.metadataBytesUsed(), key.bytes().size());
  FilterCache::UseRegistry lookup(cache, 1, segments);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, lookup.get(key)->probe(0).kind());
  EXPECT_EQ(0u, cache.counters().oversizedKeyBypasses);
}

TEST(FilterCacheTest, concurrentHitEvictAndPublish) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("concurrent");
  {
    FilterCache::UseRegistry seed(cache, 1, segments);
    auto* use = seed.get(key);
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, bitDocs(4096, 1), 1);
  }
  evictValues(cache);
  ASSERT_EQ(1u, cache.counters().unrepaidEvictions);
  {
    FilterCache::UseRegistry backedOff(cache, 1, segments);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
              backedOff.get(key)->probe(0).kind());
  }
  std::atomic<bool> start{false};

  auto publisher = [&](int32_t doc) {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 500; i++) {
      FilterCache::UseRegistry request(cache, 1, segments);
      auto* cacheUse = request.get(key);
      auto probe = cacheUse->probe(0);
      if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
        cacheUse->publishRaw(0, probe, bitDocs(4096, doc), 1);
      } else if (probe.kind() == FilterCache::Probe::Kind::HIT) {
        EXPECT_GT(probe.card(), 0);
      }
    }
  };
  auto byproduct = [&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 500; i++) {
      FilterCache::UseRegistry request(cache, 1, segments);
      request.get(key)->offerRaw(0, bitDocs(4096, 3), 1);
    }
  };
  auto evictor = [&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 500; i++) evictValues(cache);
  };

  std::thread first(publisher, 1);
  std::thread second(publisher, 2);
  std::thread third(byproduct);
  std::thread fourth(evictor);
  start.store(true, std::memory_order_release);
  first.join();
  second.join();
  third.join();
  fourth.join();
  ASSERT_NO_THROW(cache.validateForTest());
  EXPECT_GT(cache.counters().buildAttempts, 0u);
  EXPECT_GT(cache.counters().unrepaidEvictions, 0u);
  EXPECT_GT(cache.counters().populationDeferred, 0u);
  cache.clear();
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_GT(cache.counters().builds + cache.counters().byproductInserts, 0u);
}

TEST(FilterCacheTest, concurrentPublicationsFinishWithinBudget) {
  auto sample = bitDocs(4096, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;

  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  for (int32_t i = 0; i < 8; i++) {
    threads.emplace_back([&, i] {
      while (!start.load(std::memory_order_acquire)) {
      }
      FilterCache::UseRegistry request(cache, 1, segments);
      auto* use = request.get(FilterKey("parallel-" + std::to_string(i)));
      auto probe = use->probe(0);
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
      use->publishRaw(0, probe, bitDocs(4096, i + 1), 1);
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  EXPECT_LE(cache.bytesUsed(), charge);
}

TEST(FilterCacheTest, filterSupplierRoutesBypassBuildThenHit) {
  FilterCacheConfig config = testConfig();
  config.minSegmentDocs = 0;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "cache me");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto cache = writer.snapshots.readers.filterCache;
  TermQuery query("text_w", "cache");

  {
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query);
    MemPool execPool;
    auto* supplier = QueryPrep::filterSupplier(
        execPool, *weight, use, *reader, reader->segments()[0]);
    ASSERT_NE(nullptr, supplier);
    EXPECT_EQ(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(supplier));
  }

  {
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query);
    MemPool execPool;
    auto* supplier = QueryPrep::filterSupplier(
        execPool, *weight, use, *reader, reader->segments()[0]);
    ASSERT_NE(nullptr, supplier);
    EXPECT_NE(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(supplier));
    EXPECT_EQ(1, supplier->cost());
  }

  {
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query);
    MemPool execPool;
    auto* supplier = QueryPrep::filterSupplier(
        execPool, *weight, use, *reader, reader->segments()[0]);
    ASSERT_NE(nullptr, supplier);
    EXPECT_NE(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(supplier));
  }
  auto counters = cache->counters();
  EXPECT_EQ(1u, counters.builds);
  EXPECT_EQ(1u, counters.hits);
}

TEST(FilterCacheTest, disabledCacheSparseBatchRetainsPostingsFeed) {
  FilterCacheConfig config = testConfig();
  config.maxBytes = 0;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  for (int32_t doc = 0; doc < 1024; doc++) {
    addTermDoc(writer, doc == 7 ? "selected body" : "body");
  }
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  ASSERT_NE(nullptr, reader->filterCache());
  ASSERT_FALSE(reader->filterCache()->enabled());

  TermQuery selected("text_w", "selected");
  MemPool contextPool;
  Query::Context context(contextPool, *reader);
  ASSERT_NE(nullptr, context.filterUses);
  auto* use = context.getFilterUse(selected);
  ASSERT_NE(nullptr, use);
  auto* weight = selected.createWeight(context, 0);

  OwnedFilterStatsGuard stats;
  MemPool firstPool;
  auto* first = QueryPrep::filterSupplier(
      firstPool, *weight, use, *reader, reader->segments()[0],
      QueryPrep::FilterSupplierMode::SPARSE_BATCH, 64);
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(first));
  EXPECT_EQ(0, SkipStats::ownedFilterMaterializations);
  EXPECT_EQ(0, SkipStats::ownedFilterServes);

  auto before = reader->filterCache()->counters();
  MemPool secondPool;
  auto* second = QueryPrep::filterSupplier(
      secondPool, *weight, use, *reader, reader->segments()[0],
      QueryPrep::FilterSupplierMode::SPARSE_BATCH, 64);
  ASSERT_NE(nullptr, second);
  EXPECT_EQ(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(second));
  EXPECT_EQ(0, SkipStats::ownedFilterMaterializations);
  EXPECT_EQ(0, SkipStats::ownedFilterServes);
  EXPECT_EQ(reader->filterCache()->counters().misses, before.misses);

  SparseBatchPostingsFeedGuard disabledFeed(true);
  MemPool disabledContextPool;
  Query::Context disabledContext(disabledContextPool, *reader);
  auto* disabledWeight = selected.createWeight(disabledContext, 0);
  auto* disabledUse = disabledContext.getFilterUse(selected);
  MemPool disabledExecPool;
  auto* disabledSupplier = QueryPrep::filterSupplier(
      disabledExecPool, *disabledWeight, disabledUse, *reader,
      reader->segments()[0],
      QueryPrep::FilterSupplierMode::SPARSE_BATCH, 64);
  ASSERT_NE(nullptr,
            dynamic_cast<QueryPrep::DocSetSupplier*>(disabledSupplier));
  EXPECT_EQ(1, SkipStats::ownedFilterMaterializations);
  EXPECT_EQ(1, SkipStats::ownedFilterServes);
}

TEST(FilterCacheTest, sparsePostingsFeedMatchesMaterializedBatch) {
  FilterCacheConfig config = testConfig();
  config.maxBytes = 0;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  constexpr int32_t maxDoc = 2 * DocsEnumMeta::L1_DOCS;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    std::string text = "filler";
    if ((doc & 1) == 0) text += " alpha";
    if ((doc % 3) == 0) text += " beta";
    if ((doc % 3) != 1) text += " block_filter";
    if ((doc % 1000) == 0) text += " selected";
    if ((doc % 100) == 0) text += " middle";
    addTermDoc(writer, text);
  }
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto& segment = reader->segments()[0];

  struct Run {
    int64_t count = 0;
    std::vector<int32_t> docs;
    std::vector<float> scores;
    int64_t batchEngagements = 0;
    int64_t postingsFeeds = 0;
    int64_t denseWindows = 0;
    int64_t wordBlocks = 0;
  };
  auto run = [&](std::string_view filterTerm, bool scored,
                 bool disablePostingsFeed, bool disableArrayFeed) {
    SparseBatchPostingsFeedGuard postingsGuard(disablePostingsFeed);
    FilteredDisjunctionArrayFeedGuard arrayGuard(disableArrayFeed);
    bool savedStats = SkipStats::enabled;
    SkipStats::enabled = true;
    SkipStats::reset();

    MemPool pool;
    Query::Context context(pool, *reader);
    TermQuery alpha("text_w", "alpha");
    TermQuery beta("text_w", "beta");
    TermQuery selected("text_w", filterTerm);
    std::array<Query*, 2> optional{&alpha, &beta};
    std::array<Query*, 1> filters{&selected};
    BooleanQuery query({}, optional, {}, filters, 1);
    auto* weight = query.createWeight(
        context, scored ? Query::NEED_SCORES : 0);
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(pool);
    EXPECT_NE(bulk, nullptr);

    Run result;
    int32_t cursor = 0;
    while (bulk != nullptr && cursor != PostingsReader::END
           && cursor < maxDoc) {
      int32_t next;
      if (scored) {
        ScoreWindow window;
        next = bulk->scoreNextWindow(
            window, nullptr, cursor, maxDoc,
            std::numeric_limits<float>::lowest());
        result.docs.insert(
            result.docs.end(), window.docs.begin(),
            window.docs.begin() + window.size);
        result.scores.insert(
            result.scores.end(), window.scores.begin(),
            window.scores.begin() + window.size);
      } else {
        next = bulk->countNextWindow(
            result.count, nullptr, nullptr, cursor, maxDoc);
      }
      if (next == PostingsReader::END) {
        break;
      }
      EXPECT_GT(next, cursor);
      cursor = next;
    }
    result.batchEngagements = SkipStats::filteredDisjBatchEngagements;
    result.postingsFeeds =
        SkipStats::filteredDisjBatchPostingsFeedEngagements;
    result.denseWindows = SkipStats::conjDenseCountWindows;
    result.wordBlocks = SkipStats::countBulkFillWordBlocks;
    SkipStats::reset();
    SkipStats::enabled = savedStats;
    return result;
  };

  Run countPostings = run("selected", false, false, false);
  Run countArray = run("selected", false, true, false);
  Run countVirtual = run("selected", false, true, true);
  EXPECT_EQ(9, countPostings.count);
  EXPECT_EQ(countPostings.count, countArray.count);
  EXPECT_EQ(countPostings.count, countVirtual.count);
  EXPECT_GT(countPostings.batchEngagements, 0);
  EXPECT_GT(countPostings.postingsFeeds, 0);
  EXPECT_EQ(0, countArray.postingsFeeds);

  Run scorePostings = run("selected", true, false, false);
  Run scoreArray = run("selected", true, true, false);
  Run scoreVirtual = run("selected", true, true, true);
  EXPECT_EQ(9u, scorePostings.docs.size());
  EXPECT_EQ(scorePostings.docs, scoreArray.docs);
  EXPECT_EQ(scorePostings.docs, scoreVirtual.docs);
  EXPECT_EQ(scorePostings.scores, scoreArray.scores);
  EXPECT_EQ(scorePostings.scores, scoreVirtual.scores);
  EXPECT_GT(scorePostings.batchEngagements, 0);
  EXPECT_GT(scorePostings.postingsFeeds, 0);
  EXPECT_EQ(0, scoreArray.postingsFeeds);

  struct GatherRun {
    std::vector<int32_t> docs;
    std::vector<int32_t> resumes;
  };
  auto gather = [&](int32_t min, int32_t max, bool disableBlockGather) {
    FilteredDisjunctionPostingsBlockGatherGuard gatherGuard(
        disableBlockGather);
    MemPool pool;
    Query::Context context(pool, *reader);
    TermQuery blockFilter("text_w", "block_filter");
    TermQuery alpha("text_w", "alpha");
    auto scorer = [&](TermQuery& query) {
      auto* weight = query.createWeight(context, 0);
      return (TermQuery::Scorer*) weight->createScorer(pool, segment);
    };
    auto* filterScorer = scorer(blockFilter);
    std::array<TermQuery::Scorer*, 2> termScorers{
        scorer(blockFilter), scorer(alpha)};
    if (filterScorer == nullptr || termScorers[0] == nullptr
        || termScorers[1] == nullptr) {
      ADD_FAILURE() << "expected all term scorers";
      return GatherRun{};
    }
    auto* bulk = pool.make<BooleanQuery::FilteredDisjunctionBulkScorer>(
        pool, filterScorer, std::span<const int32_t>{}, termScorers,
        maxDoc, filterScorer);

    GatherRun result;
    int32_t cursor = min;
    while (cursor != PostingsReader::END && cursor < max) {
      ScoreWindow window;
      int32_t next = bulk->matchNextWindow(
          window, nullptr, cursor, max);
      result.docs.insert(
          result.docs.end(), window.docs.begin(),
          window.docs.begin() + window.size);
      result.resumes.push_back(next);
      if (next == PostingsReader::END) {
        break;
      }
      EXPECT_GT(next, cursor);
      cursor = next;
    }
    return result;
  };
  auto expectedDocs = [](int32_t min, int32_t max) {
    std::vector<int32_t> docs;
    for (int32_t doc = min; doc < max; doc++) {
      if ((doc % 3) != 1) {
        docs.push_back(doc);
      }
    }
    return docs;
  };

  std::vector<int32_t> expected = expectedDocs(0, maxDoc);
  ASSERT_GT(expected.size(), (size_t) DocsEnumMeta::L1_DOCS);
  int32_t blockBoundary =
      expected[(size_t) Postings::DOCS_BLOCK_SIZE];
  int32_t straddleMin = blockBoundary - 7;
  int32_t straddleMax = blockBoundary + 11;
  GatherRun straddledBlocks = gather(straddleMin, straddleMax, false);
  GatherRun straddledVirtual = gather(straddleMin, straddleMax, true);
  EXPECT_EQ(expectedDocs(straddleMin, straddleMax),
            straddledBlocks.docs);
  EXPECT_EQ(straddledVirtual.docs, straddledBlocks.docs);
  EXPECT_EQ((std::vector<int32_t>{PostingsReader::END}),
            straddledBlocks.resumes);
  EXPECT_EQ(straddledVirtual.resumes, straddledBlocks.resumes);

  GatherRun cappedBlocks = gather(0, maxDoc, false);
  GatherRun cappedVirtual = gather(0, maxDoc, true);
  EXPECT_EQ(expected, cappedBlocks.docs);
  EXPECT_EQ(cappedVirtual.docs, cappedBlocks.docs);
  EXPECT_EQ(
      (std::vector<int32_t>{
          expected[(size_t) DocsEnumMeta::L1_DOCS],
          PostingsReader::END}),
      cappedBlocks.resumes);
  EXPECT_EQ(cappedVirtual.resumes, cappedBlocks.resumes);

  Run middleCount = run("middle", false, false, false);
  EXPECT_EQ(0, middleCount.batchEngagements);
  EXPECT_GT(middleCount.denseWindows, 0);
  EXPECT_GT(middleCount.wordBlocks, 0);
}

TEST(FilterCacheTest, ownedAndBorrowedValuesComposeDeletesIdentically) {
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
  IndexWriter writer(writerSnapshots);
  for (int32_t doc = 0; doc < 8; doc++) {
    addIdTermDoc(writer, std::to_string(doc), "selected");
  }
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("1", 1);
  deletes.deleteId("3", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = writer.snapshots.readers.getReader(0);
  ASSERT_EQ(1u, reader->segments().size());
  ASSERT_NE(nullptr, reader->segments()[0].liveDocs());
  auto identities = readerIdentitiesForTest(*reader);

  FilterCacheConfig offConfig = testConfig();
  offConfig.maxBytes = 0;
  FilterCache off(offConfig);
  FilterCache::UseRegistry ownedRequest(&off, *reader);
  auto* ownedUse = ownedRequest.get(FilterKey("delete-parity"));
  auto ownedProbe = ownedUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, ownedProbe.kind());
  ownedUse->adoptOwnedRaw(
      0, ownedProbe, docs(reader->segments()[0].maxDoc(), {0, 1, 2, 3}));
  DocSet* owned = ownedUse->effectiveDocSet(0, *reader);

  FilterCacheConfig onConfig = testConfig();
  onConfig.admissionThreshold = 1;
  FilterCache on(onConfig);
  ASSERT_TRUE(on.onReaderPublished(reader->coreGen(), identities));
  FilterCache::UseRegistry borrowedRequest(on, *reader);
  auto* borrowedUse = borrowedRequest.get(FilterKey("delete-parity"));
  auto borrowedProbe = borrowedUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, borrowedProbe.kind());
  borrowedUse->publishRaw(
      0, borrowedProbe,
      docs(reader->segments()[0].maxDoc(), {0, 1, 2, 3}), 1);
  DocSet* borrowed = borrowedUse->effectiveDocSet(0, *reader);

  ASSERT_NE(nullptr, owned);
  ASSERT_NE(nullptr, borrowed);
  EXPECT_EQ(borrowed->card(), owned->card());
  for (int32_t doc = 0; doc < reader->segments()[0].maxDoc(); doc++) {
    EXPECT_EQ(borrowed->get(doc), owned->get(doc)) << doc;
  }
  EXPECT_TRUE(owned->get(0));
  EXPECT_TRUE(owned->get(2));
  EXPECT_FALSE(owned->get(1));
  EXPECT_FALSE(owned->get(3));
}

TEST(FilterCacheTest, buildRaceLoserOwnsWithoutPublishingOrReprobing) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("build-race-owned");

  FilterCache::UseRegistry builder(cache, 1, segments);
  auto* builderUse = builder.get(key);
  auto builderProbe = builderUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, builderProbe.kind());

  OwnedFilterStatsGuard stats;
  FilterCache::UseRegistry loser(cache, 1, segments);
  auto* loserUse = loser.get(key);
  auto loserProbe = loserUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, loserProbe.kind());
  loserUse->adoptOwnedRaw(0, loserProbe, docs(1024, {5, 9}));
  EXPECT_EQ(1, SkipStats::ownedFilterMaterializations);
  EXPECT_EQ(1, SkipStats::ownedFilterServes);
  EXPECT_EQ(0u, cache.counters().builds);
  EXPECT_EQ(0u, cache.bytesUsed());

  uint64_t misses = cache.counters().misses;
  auto localHit = loserUse->probe(0);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, localHit.kind());
  EXPECT_TRUE(localHit.docSet()->get(5));
  EXPECT_EQ(misses, cache.counters().misses);
  EXPECT_EQ(1, SkipStats::ownedFilterMaterializations);
  EXPECT_EQ(2, SkipStats::ownedFilterServes);

  builderUse->publishRaw(0, builderProbe, docs(1024, {5, 9}), 1);
  EXPECT_EQ(1u, cache.counters().builds);
  EXPECT_GT(cache.bytesUsed(), 0u);
}

TEST(FilterCacheTest, observedExactSourceDoesNotPricePlainBooleanFilter) {
  auto config = testConfig(); config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(snapshots);
  addTermDoc(writer, "selected"); addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  TermQuery selected("text_w", "selected");
  QueryPrep::ExactDomainPlan plan;
  plan.add(*selected.createWeight(context, 0), context.getFilterUse(selected));
  std::array<Query*, 1> filters{&selected};
  BooleanQuery query({}, {}, {}, filters);
  auto* weight = query.createWeight(context, 0);
  FilterCache::UseRegistry::SegmentScope pass(*context.filterUses, 0);
  EXPECT_NE(nullptr, weight->scorerSupplier(pool, reader->segments()[0]));
  EXPECT_EQ(1u, reader->filterCache()->counters().buildAttempts);
  EXPECT_EQ(1u, reader->filterCache()->counters().assumedCharges);
}

TEST(FilterCacheTest, restrictedExactDomainEarnsCanonicalRawMembership) {
  test::CollectionHelper helper("funded_exact_domain");
  auto config = testConfig();
  config.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  ASSERT_TRUE(helper.indexAll({test::flatdoc("id", "0", "text_w", "selected"),
      test::flatdoc("id", "1", "text_w", "selected"),
      test::flatdoc("id", "2", "text_w", "other")}, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  TermQuery query("text_w", "selected");
  auto run = [&](IndexReader& view, DocSet* domain, bool timed) {
    MemPool pool;
    Query::Context context(pool, view);
    auto* use = context.getFilterUse(query);
    QueryPrep::ExactDomainPlan plan;
    plan.add(*query.createWeight(context, 0), use);
    std::optional<FilterCache::UseRegistry::SegmentScope> scope;
    if (timed) scope.emplace(*context.filterUses, 0);
    auto result = plan.produce(pool, view, view.segments()[0], domain);
    return result.available ? result.docs.get()->card() : -1;
  };
  auto first = docs(3, {0});
  for (int i = 0; i < 20; i++) EXPECT_EQ(-1, run(*reader, first.get(), false));
  EXPECT_EQ(0u, cache->counters().buildAttempts);
  EXPECT_EQ(0u, cache->counters().assumedCharges);
  EXPECT_EQ(-1, run(*reader, first.get(), true));
  EXPECT_EQ(1, run(*reader, first.get(), true));
  ASSERT_EQ(1u, cache->counters().builds);
  auto other = docs(3, {1, 2});
  EXPECT_EQ(1, run(*reader, other.get(), false));
  auto attempts = cache->counters().buildAttempts;
  ASSERT_TRUE(helper.deleteById("0", UpdateMessage::COMMIT).success);
  auto deleted = helper.getIndexWriter()->snapshots.readers.getReader();
  auto both = docs(3, {0, 1});
  EXPECT_EQ(1, run(*deleted, both.get(), false));
  EXPECT_EQ(attempts, cache->counters().buildAttempts);
}

TEST(FilterCacheTest, uncacheableFilterStaysPostingsBacked) {
  FilterCacheConfig config = testConfig();
  config.maxBytes = 0;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "selected");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();

  TermQuery term("text_w", "selected");
  UncacheableQuery query(term);
  MemPool contextPool;
  Query::Context context(contextPool, *reader);
  EXPECT_EQ(nullptr, context.getFilterUse(query));
  auto* weight = query.createWeight(context, 0);
  MemPool execPool;
  auto* supplier = QueryPrep::filterSupplier(
      execPool, *weight, context.getFilterUse(query), *reader,
      reader->segments()[0],
      QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE);
  ASSERT_NE(nullptr, supplier);
  EXPECT_EQ(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(supplier));
}

TEST(FilterCacheTest, requestStableFilterOwnsAndDeduplicatesWithoutSharedTraffic) {
  LuxirNode node;
  luxir::test::CollectionHelper helper(node, "request_stable_filter");
  ASSERT_TRUE(helper.indexAll({luxir::test::flatdoc("when_dt", 1)},
                              UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto* cache = reader->filterCache();
  NumericPredicateQuery query("when_dt", 1, 1, FilterKeyScope::REQUEST_STABLE);
  NumericPredicateQuery same("when_dt", 1, 1, FilterKeyScope::REQUEST_STABLE);
  MemPool pool;
  Query::Context context(pool, *reader);
  EXPECT_FALSE(context.planningContext().lookupExistingFilterUse(query, true));
  auto* use = context.getFilterUse(query);
  ASSERT_NE(nullptr, use);
  EXPECT_EQ(use, context.getFilterUse(same));
  OwnedFilterStatsGuard stats;
  for (auto* q : {&query, &same}) {
    QueryPrep::materializeEffectiveFilter(
        *q->createWeight(context, 0), context.getFilterUse(*q),
        *reader, reader->segments()[0], nullptr);
  }
  EXPECT_EQ(1, SkipStats::ownedFilterMaterializations);
  EXPECT_EQ(0u, cache->entryCountForTest());
  EXPECT_EQ(0u, cache->counters().admissions);
  EXPECT_EQ(0u, cache->counters().builds);
  EXPECT_EQ(0u, cache->counters().misses);
}

TEST(FilterCacheTest, weightCountShortcutsAreNarrowAndTransparent) {
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
  IndexWriter writer(writerSnapshots);
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "alpha");
  addTermDoc(writer, "beta");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  MemPool pool;
  Query::Context context(pool, *reader);

  auto metadataCount = [&](Query::Weight* weight, DocSet* domain = nullptr) -> std::optional<int64_t> {
    auto shortcut = weight->countShortcut(segment, domain);
    if (shortcut.how != Query::CountHow::METADATA) return std::nullopt;
    return shortcut.maxMatches;
  };

  AllQuery all;
  MatchNoDocsQuery none;
  TermQuery alpha("text_w", "alpha");
  ConstantScoreQuery constant(&alpha, 3.0f);
  BoostQuery boost(&alpha, 2.0f);
  ForcePrepareQuery prepared(&alpha);
  auto* allWeight = all.createWeight(context, 0);
  auto* noneWeight = none.createWeight(context, 0);
  auto* alphaWeight = alpha.createWeight(context, 0);
  auto* constantWeight = constant.createWeight(context, 0);
  auto* boostWeight = boost.createWeight(context, 0);
  auto* preparedWeight = prepared.createWeight(context, 0);

  EXPECT_EQ(3, *metadataCount(allWeight, nullptr));
  EXPECT_EQ(0, *metadataCount(noneWeight, nullptr));
  EXPECT_EQ(2, *metadataCount(alphaWeight, nullptr));
  EXPECT_EQ(2, *metadataCount(constantWeight, nullptr));
  EXPECT_EQ(2, *metadataCount(boostWeight, nullptr));
  EXPECT_EQ(2, *metadataCount(preparedWeight, nullptr));

  auto restricted = docs(3, {0, 2});
  EXPECT_EQ(2, metadataCount(allWeight, restricted.get()));
  EXPECT_EQ(0, *metadataCount(noneWeight, restricted.get()));
  EXPECT_FALSE(metadataCount(alphaWeight, restricted.get()).has_value());

  TermQuery beta("text_w", "beta");
  TermQuery ghost("text_w", "ghost");
  TermQuery wraith("text_w", "wraith");
  std::array<Query*, 2> alphaOrGhost{&alpha, &ghost};
  std::array<Query*, 2> alphaOrBeta{&alpha, &beta};
  std::array<Query*, 2> ghostOrWraith{&ghost, &wraith};
  std::array<Query*, 1> alphaOnly{&alpha};
  std::array<Query*, 1> betaOnly{&beta};
  BooleanQuery deadClauseUnion({}, alphaOrGhost, {}, {}, 1);
  BooleanQuery liveUnion({}, alphaOrBeta, {}, {}, 1);
  BooleanQuery deadUnion({}, ghostOrWraith, {}, {});
  BooleanQuery mandOpt(alphaOnly, betaOnly, {}, {});
  BooleanQuery gatedOpt(alphaOnly, betaOnly, {}, {}, 1);
  BooleanQuery excluded(alphaOnly, {}, betaOnly, {});
  auto* deadClauseUnionWeight = deadClauseUnion.createWeight(context, 0);
  auto* liveUnionWeight = liveUnion.createWeight(context, 0);
  auto* deadUnionWeight = deadUnion.createWeight(context, 0);
  auto* mandOptWeight = mandOpt.createWeight(context, 0);
  auto* gatedOptWeight = gatedOpt.createWeight(context, 0);
  auto* excludedWeight = excluded.createWeight(context, 0);

  // A union whose other clauses are empty counts as its one live clause;
  // two live clauses have unknown overlap. Dropped (non-gating) optionals
  // leave the lone required clause's count; a gating optional group or a
  // prohibited clause needs execution.
  EXPECT_EQ(2, *metadataCount(deadClauseUnionWeight, nullptr));
  EXPECT_FALSE(metadataCount(liveUnionWeight, nullptr).has_value());
  EXPECT_EQ(0, *metadataCount(deadUnionWeight, nullptr));
  EXPECT_EQ(0, *metadataCount(deadUnionWeight, restricted.get()));
  EXPECT_EQ(2, *metadataCount(mandOptWeight, nullptr));
  EXPECT_FALSE(metadataCount(gatedOptWeight, nullptr).has_value());
  EXPECT_FALSE(metadataCount(excludedWeight, nullptr).has_value());
  EXPECT_FALSE(metadataCount(deadClauseUnionWeight, restricted.get()).has_value());

  ExistsQuery exists("text_w");
  ExistsQuery absentField("other_w");
  auto* existsWeight = exists.createWeight(context, 0);
  auto* absentFieldWeight = absentField.createWeight(context, 0);
  EXPECT_EQ(3, *metadataCount(existsWeight, nullptr));
  EXPECT_EQ(0, *metadataCount(absentFieldWeight, nullptr));
  EXPECT_EQ(0, *metadataCount(absentFieldWeight, restricted.get()));
  EXPECT_FALSE(metadataCount(existsWeight, restricted.get()).has_value());
}

TEST(FilterCacheTest,
     wholeMembershipComposesIncomingDomainAndGuardsPublication) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "alpha");
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "beta");
  addTermDoc(writer, "alpha beta gamma");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  TermQuery alpha("text_w", "alpha");
  TermQuery beta("text_w", "beta");
  std::array<Query*, 2> required{&alpha, &beta};
  BooleanQuery query(required, {}, {}, {});
  auto incoming = docs(5, {0, 1, 3});

  OwnedFilterStatsGuard stats;
  MemPool firstPool;
  Query::Context firstContext(firstPool, *reader);
  auto* firstWeight = query.createWeight(firstContext, 0);
  auto* firstUse = firstContext.getFilterUse(
      query, FilterCache::AdmissionLane::WHOLE);
  QueryPrep::WholeMembershipPlan firstPlan(
      *firstWeight, firstUse, firstContext.filterUses);
  auto built = test::resolveWholeMembershipForTests(firstPlan, *reader, segment, incoming.get());
  ASSERT_TRUE(built.available);
  EXPECT_EQ(1, built.count);
  ASSERT_NE(nullptr, built.docs.get());
  EXPECT_EQ(1, built.docs.get()->card());
  ASSERT_NE(nullptr, firstUse->rawDocSet(0));
  EXPECT_EQ(3, firstUse->rawDocSet(0)->card());
  EXPECT_EQ(1, SkipStats::wholeCountBuilds);

  SkipStats::reset();
  MemPool hitPool;
  Query::Context hitContext(hitPool, *reader);
  auto* hitWeight = query.createWeight(hitContext, 0);
  auto* hitUse = hitContext.getFilterUse(
      query, FilterCache::AdmissionLane::WHOLE);
  QueryPrep::WholeMembershipPlan hitPlan(
      *hitWeight, hitUse, hitContext.filterUses);
  auto hit = test::resolveWholeMembershipForTests(hitPlan, *reader, segment, incoming.get());
  ASSERT_TRUE(hit.available);
  EXPECT_EQ(1, hit.count);
  EXPECT_EQ(1, SkipStats::wholeCountHits);
  // The resolved membership is the incoming-domain composition, owned by the
  // result rather than memoized in the Use.
  ASSERT_NE(nullptr, hit.docs.get());
  EXPECT_TRUE(hit.docs.isDeliverable());
  DomainHandle composed = hitUse->effectiveDomain(0, *reader, incoming.get());
  ASSERT_NE(nullptr, composed.get());
  EXPECT_EQ(hit.docs.get()->card(), composed.get()->card());

  auto emptyDomain = docs(5, {});
  auto allDeleted = test::resolveWholeMembershipForTests(hitPlan,
      *reader, segment, emptyDomain.get());
  ASSERT_TRUE(allDeleted.available);
  EXPECT_EQ(0, allDeleted.count);

  TermQuery gamma("text_w", "gamma");
  std::array<Query*, 2> dependentRequired{&alpha, &gamma};
  BooleanQuery dependentQuery(dependentRequired, {}, {}, {});
  MemPool dependentPool;
  Query::Context dependentContext(dependentPool, *reader);
  auto* dependentWeight = dependentQuery.createWeight(dependentContext, 0);
  auto* dependentUse = dependentContext.getFilterUse(
      dependentQuery, FilterCache::AdmissionLane::WHOLE);
  QueryPrep::WholeMembershipPlan dependentPlan(
      *dependentWeight, dependentUse, dependentContext.filterUses,
      PreparedDomainDependence::PREPARE_DOMAIN);
  EXPECT_THROW(
      test::resolveWholeMembershipForTests(dependentPlan, *reader, segment, nullptr), std::logic_error);

  MemPool retryPool;
  Query::Context retryContext(retryPool, *reader);
  auto* retryWeight = dependentQuery.createWeight(retryContext, 0);
  auto* retryUse = retryContext.getFilterUse(
      dependentQuery, FilterCache::AdmissionLane::WHOLE);
  QueryPrep::WholeMembershipPlan retryPlan(
      *retryWeight, retryUse, retryContext.filterUses,
      PreparedDomainDependence::QUERY_CANONICAL, QueryPrep::WholeMembershipConsumer::FIELD_SORT);
  auto retry = test::resolveWholeMembershipForTests(retryPlan, *reader, segment, nullptr);
  EXPECT_FALSE(retry.available);
  auto claim = retryUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
  retryUse->publishRaw(0, claim, QueryPrep::materializeRawFilter(*retryWeight, nullptr, segment), 1000);
  retry = test::resolveWholeMembershipForTests(retryPlan, *reader, segment, nullptr);
  ASSERT_TRUE(retry.available);
  EXPECT_EQ(1, retry.count);
}

TEST(FilterCacheTest, localWholeMembershipUsesAnEvictedProduction) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(snapshots, {}, nullptr);
  int32_t id = 0;
  for (auto text : {"alpha beta", "alpha", "alpha beta", "beta", "alpha beta"}) {
    addIdTermDoc(writer, std::to_string(id++), text);
  }
  addIdTermDoc(writer, "deleted", "other");
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("deleted", 1);
  deletes.deleteId("4", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = snapshots.readers.getReader();
  auto cache = snapshots.readers.filterCache;
  TermQuery alpha("text_w", "alpha"), beta("text_w", "beta");
  std::array<Query*, 2> required{&alpha, &beta};
  BooleanQuery query(required, {}, {}, {});
  {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    use->publishRaw(0, probe, QueryPrep::materializeRawFilter(
        *query.createWeight(context, 0), nullptr, reader->segments()[0]), 1000);
  }
  evictValues(*cache);
  MemPool pool;
  RequestMemTracker memory(0);
  google::protobuf::Arena arena;
  auto* planning = luxir::arenaCreate<Query::PlanningContext>(arena, pool, *reader,
      Query::PlanningContext::Limits{}, nullptr, FilterKeyContext{}, nullptr, &memory);
  auto& context = *Query::Context::create(&arena, *planning);
  auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
  {
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, probe.kind());
    EXPECT_EQ(3, probe.priorCardinality());
    auto moved = std::move(probe);
    FilterCache::Probe assigned;
    assigned = std::move(moved);
    EXPECT_EQ(3, assigned.priorCardinality());
  }
  QueryPrep::WholeMembershipPlan plan(*query.createWeight(context, 0), use, context.filterUses,
      PreparedDomainDependence::QUERY_CANONICAL, QueryPrep::WholeMembershipConsumer::TOP_K_COUNT);
  OwnedFilterStatsGuard stats;
  auto before = cache->counters();
  auto result = test::resolveWholeMembershipForTests(plan, *reader, reader->segments()[0], nullptr, nullptr, 128);
  ASSERT_TRUE(result.available);
  EXPECT_EQ(2, result.count);
  EXPECT_EQ(nullptr, result.docs.get());
  EXPECT_EQ(nullptr, use->rawDocSet(0));
  EXPECT_EQ(0u, use->ownedBytesForTest());
  EXPECT_EQ(0u, memory.bytes());
  EXPECT_EQ(0u, memory.peak());
  auto again = test::resolveWholeMembershipForTests(plan, *reader, reader->segments()[0], nullptr, nullptr, 128);
  EXPECT_EQ(result.count, again.count);
  EXPECT_EQ(nullptr, again.docs.get());
  auto after = cache->counters();
  EXPECT_EQ(before.buildAttempts, after.buildAttempts);
  EXPECT_EQ(before.builds, after.builds);
  EXPECT_EQ(before.hits, after.hits);
  EXPECT_EQ(before.assumedCharges, after.assumedCharges);
  EXPECT_EQ(before.assumedCredits, after.assumedCredits);
  EXPECT_EQ(before.outstandingDebtNanos, after.outstandingDebtNanos);
  EXPECT_EQ(before.byproductInserts, after.byproductInserts);
  EXPECT_EQ(0u, after.residentBytes);
  EXPECT_EQ(2, SkipStats::wholeTopKCountLocalCounts);
  EXPECT_EQ(0, SkipStats::bulkDomainWindowsFed);
}

TEST(FilterCacheTest, priorCardinalityAnswersCountsWithoutMembershipOrSuppliers) {
  auto config = testConfig(); config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(snapshots, {}, nullptr);
  for (auto text : {"alpha beta", "alpha", "alpha beta", "beta", "alpha beta"}) addTermDoc(writer, text);
  writer.commit();
  auto reader = snapshots.readers.getReader();
  auto cache = snapshots.readers.filterCache;
  auto& segment = reader->segments()[0];
  TermQuery alpha("text_w", "alpha"), beta("text_w", "beta");
  std::array<Query*, 2> required{&alpha, &beta};
  BooleanQuery query(required, {}, {}, {});
  {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
    auto claim = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
    use->publishRaw(0, claim, QueryPrep::materializeRawFilter(
        *query.createWeight(context, 0), nullptr, segment), 1000);
  }
  evictValues(*cache);
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
  { auto probe = use->probe(0); ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, probe.kind()); }
  auto before = cache->counters();
  auto* weight = query.createWeight(context, 0);
  OwnedFilterStatsGuard stats;
  int32_t suppliers = 0;
  auto supplierFor = [&](Query::Weight& source) {
    suppliers++;
    return source.scorerSupplier(pool, segment);
  };
  for (auto consumer : {QueryPrep::WholeMembershipConsumer::COUNT, QueryPrep::WholeMembershipConsumer::TOP_K_COUNT}) {
    QueryPrep::WholeMembershipPlan plan(*weight, use, context.filterUses,
        PreparedDomainDependence::QUERY_CANONICAL, consumer);
    auto result = plan.resolve(*reader, segment, nullptr, nullptr, supplierFor);
    ASSERT_TRUE(result.available);
    EXPECT_EQ(3, result.count);
    EXPECT_EQ(nullptr, result.docs.get());
    EXPECT_EQ(nullptr, use->rawDocSet(0));
    auto restricted = docs(segment.maxDoc(), {1, 2});
    EXPECT_TRUE(plan.canRecordCount(segment, nullptr));
    EXPECT_FALSE(plan.canRecordCount(segment, restricted.get()));
    EXPECT_FALSE(plan.resolve(*reader, segment, restricted.get(), nullptr, supplierFor).available);
    test::SearchOverridesGuard guard(QueryPrep::disableWholeMembershipBypassReuseForTests,
        QueryPrep::disableCanonicalCountFactsForTests);
    QueryPrep::disableCanonicalCountFactsForTests = true;
    QueryPrep::disableWholeMembershipBypassReuseForTests = true;
    EXPECT_FALSE(plan.resolve(*reader, segment, nullptr, nullptr, supplierFor).available);
  }
  QueryPrep::WholeMembershipPlan fieldSort(*weight, use, context.filterUses,
      PreparedDomainDependence::QUERY_CANONICAL, QueryPrep::WholeMembershipConsumer::FIELD_SORT);
  EXPECT_FALSE(fieldSort.resolve(*reader, segment, nullptr, nullptr, supplierFor).available);
  ForcePrepareQuery forced(&query);
  auto* preparedWeight = forced.createWeight(context, 0);
  Query::Weight::PrepareContext prepareContext{*reader, {}, nullptr, nullptr};
  auto prepared = preparedWeight->prepare(prepareContext);
  QueryPrep::WholeMembershipPlan preparedPlan(*preparedWeight, use, context.filterUses);
  EXPECT_FALSE(preparedPlan.canRecordCount(segment, nullptr));
  EXPECT_FALSE(preparedPlan.resolve(*reader, segment, nullptr, prepared.get(), supplierFor).available);
  EXPECT_EQ(0, suppliers);
  EXPECT_EQ(2, SkipStats::wholeMembershipCardinalityCounts);
  EXPECT_EQ(0, SkipStats::wholeTopKCountLocalCounts);
  EXPECT_EQ(before, cache->counters());
}

TEST(FilterCacheTest, localWholeMembershipRequiresAPriorProduction) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), testConfig());
  IndexWriter writer(snapshots, {}, nullptr);
  for (auto text : {"alpha beta", "alpha", "beta"}) addTermDoc(writer, text);
  writer.commit();
  auto reader = snapshots.readers.getReader();
  TermQuery alpha("text_w", "alpha"), beta("text_w", "beta");
  std::array<Query*, 2> required{&alpha, &beta};
  BooleanQuery query(required, {}, {}, {});
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
  {
    auto probe = use->probe(0);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, probe.kind());
    EXPECT_FALSE(probe.priorCardinality());
  }
  auto* weight = query.createWeight(context, 0);
  OwnedFilterStatsGuard stats;
  for (auto* selected : {use, (FilterCache::Use*)nullptr}) {
    QueryPrep::WholeMembershipPlan plan(*weight, selected, context.filterUses,
        PreparedDomainDependence::QUERY_CANONICAL, QueryPrep::WholeMembershipConsumer::TOP_K_COUNT);
    EXPECT_FALSE(test::resolveWholeMembershipForTests(plan, *reader, reader->segments()[0], nullptr, nullptr, 128).available);
  }
  EXPECT_EQ(0, SkipStats::wholeTopKCountLocalCounts);
}

TEST(FilterCacheTest, localWholeMembershipScreensExactSizeAndYieldBoundaries) {
  struct Case { int32_t matches; int32_t candidates; bool local; };
  constexpr std::array cases{Case{39, 312, false}, Case{40, 320, true}, Case{40, 321, false}, Case{41, 320, true}};
  auto config = testConfig(); config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(snapshots, {}, nullptr);
  for (int32_t doc = 0; doc < 5120; doc++) {
    std::string text = "other";
    for (size_t i = 0; i < cases.size(); i++) {
      const auto& c = cases[i];
      if (doc < c.candidates) text += " a" + std::to_string(i);
      if (doc < c.matches || (doc >= c.candidates && doc < 2*c.candidates-c.matches)) text += " b" + std::to_string(i);
    }
    addTermDoc(writer, text);
  }
  addIdTermDoc(writer, "deleted", "other");
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("deleted", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = snapshots.readers.getReader();
  auto cache = snapshots.readers.filterCache;
  auto& segment = reader->segments()[0];
  for (size_t i = 0; i < cases.size(); i++) {
    SCOPED_TRACE(i);
    std::string aTerm = "a" + std::to_string(i), bTerm = "b" + std::to_string(i);
    TermQuery a("text_w", aTerm), b("text_w", bTerm);
    std::array<Query*, 2> required{&a, &b};
    BooleanQuery query(required, {}, {}, {});
    {
      MemPool pool;
      Query::Context context(pool, *reader);
      auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
      auto probe = use->probe(0);
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
      use->publishRaw(0, probe, QueryPrep::materializeRawFilter(*query.createWeight(context, 0), nullptr, segment), 1000);
    }
    evictValues(*cache);
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context, 0);
    EXPECT_EQ(cases[i].candidates, weight->countShortcut(segment, nullptr).maxMatches);
    auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
    QueryPrep::WholeMembershipPlan plan(*weight, use, context.filterUses,
        PreparedDomainDependence::QUERY_CANONICAL, QueryPrep::WholeMembershipConsumer::TOP_K_COUNT);
    if (i == 1) {
      auto restricted = docs(segment.maxDoc(), {0, 1});
      EXPECT_FALSE(test::resolveWholeMembershipForTests(plan, *reader, segment, restricted.get(), nullptr, 128).available);
      EXPECT_FALSE(test::resolveWholeMembershipForTests(plan, *reader, segment, nullptr, nullptr, 128, 7).available);
      EXPECT_FALSE(test::resolveWholeMembershipForTests(plan, *reader, segment, nullptr, nullptr, 128, 0).available);
      test::SearchOverridesGuard guard(QueryPrep::disableWholeMembershipBypassReuseForTests);
      QueryPrep::disableWholeMembershipBypassReuseForTests = true;
      EXPECT_FALSE(test::resolveWholeMembershipForTests(plan, *reader, segment, nullptr, nullptr, 128).available);
    }
    auto result = test::resolveWholeMembershipForTests(plan, *reader, segment, nullptr, nullptr, 128);
    EXPECT_EQ(cases[i].local, result.available);
    if (result.available) { EXPECT_EQ(cases[i].matches, result.count); }
  }
}

TEST(FilterCacheTest, priorCardinalityIncludesByproductsAndRejectedPublications) {
  for (int mode = 0; mode < 4; mode++) {
    SCOPED_TRACE(mode);
    auto config = testConfig(); config.admissionThreshold = 1;
    FilterCache cache(config);
    std::array segments{FilterCache::SegmentIdentity{41, 128}};
    cache.onReaderPublished(1, segments);
    FilterKey key("prior-cardinality");
    int32_t count = mode == 2 ? 128 : mode == 3 ? 0 : 3;
    {
      FilterCache::UseRegistry request(cache, 1, segments);
      auto* use = request.get(key);
      auto probe = use->probe(0);
      ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
      auto value = std::make_unique<RAMBitDocSet>(128);
      for (int32_t doc = 0; doc < count; doc++) value->mutableBits().set(doc);
      if (mode == 1 || mode == 3) use->publishRawByproduct(0, probe, std::move(value), 1000);
      else use->publishRaw(0, probe, std::move(value), 1000);
    }
    evictValues(cache);
    {
      FilterCache::UseRegistry request(cache, 1, segments);
      auto probe = request.get(key)->probe(0, FilterCache::Work::assumed().observedOnly());
      EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, probe.kind());
      EXPECT_EQ(count, probe.priorCardinality());
      EXPECT_EQ(count, ledgerFor(cache, key).rawCardinality);
      EXPECT_EQ(mode == 1 || mode == 3 ? 0u : 1000u, cache.counters().outstandingDebtNanos);
    }
    cache.clear();
    FilterCache::UseRegistry request(cache, 1, segments);
    EXPECT_FALSE(request.get(key)->probe(0, FilterCache::Work::assumed().observedOnly()).priorCardinality());
  }
}

TEST(FilterCacheTest, failedLocalWholeCountReleasesTheClaimWithoutInvestment) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(snapshots, {}, nullptr);
  for (auto text : {"alpha beta", "alpha", "beta"}) addTermDoc(writer, text);
  addIdTermDoc(writer, "deleted", "other");
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("deleted", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = snapshots.readers.getReader();
  auto cache = snapshots.readers.filterCache;
  auto& segment = reader->segments()[0];
  PrefixQuery alpha("text_w", "alph", 1);
  TermQuery beta("text_w", "beta");
  std::array<Query*, 2> required{&alpha, &beta};
  BooleanQuery query(required, {}, {}, {});
  {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
    auto claim = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
    use->publishRaw(0, claim, docs(segment.maxDoc(), {0}), 1000);
  }
  evictValues(*cache);
  RequestMemTracker memory(1024);
  MemPool pool;
  google::protobuf::Arena arena;
  auto* planning = luxir::arenaCreate<Query::PlanningContext>(arena, pool, *reader,
      Query::PlanningContext::Limits{}, nullptr, FilterKeyContext{}, nullptr, &memory);
  auto& context = *Query::Context::create(&arena, *planning);
  auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
  auto* raw = query.createWeight(context, 0);
  QueryPrep::WholeMembershipPlan plan(*raw, use, context.filterUses,
      PreparedDomainDependence::QUERY_CANONICAL, QueryPrep::WholeMembershipConsumer::TOP_K_COUNT);
  auto before = cache->counters();
  memory.charge(1024, "domain variants", "retained domain");
  EXPECT_THROW(test::resolveWholeMembershipForTests(plan, *reader, segment, nullptr, nullptr, 128), ResourceExhaustedError);
  memory.release(1024);
  EXPECT_EQ(before.buildAttempts, cache->counters().buildAttempts);
  EXPECT_EQ(before.outstandingDebtNanos, cache->counters().outstandingDebtNanos);
  auto result = test::resolveWholeMembershipForTests(plan, *reader, segment, nullptr, nullptr, 128);
  EXPECT_TRUE(result.available);
  EXPECT_EQ(1, result.count);
  EXPECT_EQ(nullptr, result.docs.get());
  EXPECT_EQ(nullptr, use->rawDocSet(0));
  EXPECT_EQ(0u, use->ownedBytesForTest());
  EXPECT_EQ(0u, memory.bytes());
  EXPECT_EQ(before.builds, cache->counters().builds);
  EXPECT_EQ(before.outstandingDebtNanos, cache->counters().outstandingDebtNanos);
}

TEST(FilterCacheTest, wholeAdmissionDiscriminatesSchemaGeneration) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "alpha");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  TermQuery alpha("text_w", "alpha");
  TermQuery beta("text_w", "beta");
  std::array<Query*, 2> required{&alpha, &beta};
  BooleanQuery query(required, {}, {}, {});

  MemPool firstPool;
  Query::Context first(
      firstPool, *reader, {}, nullptr,
      FilterKeyContext{.schemaGen = 3, .timeZone = {}});
  auto* firstUse = first.getFilterUse(
      query, FilterCache::AdmissionLane::WHOLE);
  MemPool secondPool;
  Query::Context second(
      secondPool, *reader, {}, nullptr,
      FilterKeyContext{.schemaGen = 4, .timeZone = {}});
  auto* secondUse = second.getFilterUse(
      query, FilterCache::AdmissionLane::WHOLE);

  EXPECT_NE(firstUse->entryIdentityForTest(),
            secondUse->entryIdentityForTest());
  EXPECT_EQ(2u, reader->filterCache()->entryCountForTest());
}

TEST(FilterCacheTest, ownedSupplierPredicateIsModeSpecific) {
  FilterCacheConfig config = testConfig();
  config.maxBytes = 0;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  constexpr int32_t maxDoc = 1024;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    std::string terms = "body";
    if (doc < 4) terms += " sparse";
    if (doc < 24) terms += " middle";
    if (doc < 40) terms += " over";
    addTermDoc(writer, terms);
  }
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  constexpr int32_t sparseInverse = 64;
  ASSERT_EQ(32, DocSetBuilder::arrayLimitFor(maxDoc));

  auto isOwned = [&](std::string_view term,
                     QueryPrep::FilterSupplierMode mode) {
    TermQuery query("text_w", term);
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query);
    MemPool execPool;
    auto* supplier = QueryPrep::filterSupplier(
        execPool, *weight, use, *reader, reader->segments()[0],
        mode, sparseInverse);
    return dynamic_cast<QueryPrep::DocSetSupplier*>(supplier) != nullptr;
  };

  EXPECT_FALSE(isOwned(
      "sparse", QueryPrep::FilterSupplierMode::DENSITY_ROUTED));
  EXPECT_FALSE(isOwned(
      "sparse", QueryPrep::FilterSupplierMode::SPARSE_BATCH));
  EXPECT_FALSE(isOwned(
      "middle", QueryPrep::FilterSupplierMode::SPARSE_BATCH));
  EXPECT_FALSE(isOwned(
      "middle", QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE));
  EXPECT_FALSE(isOwned(
      "over", QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE));

  {
    SparseBatchPostingsFeedGuard disabledFeed(true);
    EXPECT_TRUE(isOwned(
        "sparse", QueryPrep::FilterSupplierMode::SPARSE_BATCH));
  }
}

TEST(FilterCacheTest, requestOwnedArraysStayWithinMultiFilterBound) {
  FilterCacheConfig config = testConfig();
  config.maxBytes = 0;
  FilterCache cache(config);
  constexpr int32_t maxDoc = 1024;
  constexpr int32_t filterCount = 4;
  std::array segments{FilterCache::SegmentIdentity{1, maxDoc}};
  FilterCache::UseRegistry request(&cache, 1, segments);

  for (int32_t filter = 0; filter < filterCount; filter++) {
    auto* use = request.get(
        FilterKey("owned-memory-" + std::to_string(filter)));
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BYPASS, probe.kind());
    DocSetBuilder builder(maxDoc);
    for (int32_t doc = 0; doc < DocSetBuilder::arrayLimitFor(maxDoc); doc++) {
      builder.add(doc);
    }
    use->adoptOwnedRaw(0, probe, builder.build());
  }

  size_t perFilterCap = sizeof(ArrDocSet)
      + (size_t) DocSetBuilder::arrayLimitFor(maxDoc) * sizeof(int32_t);
  EXPECT_GT(request.ownedBytesForTest(), 0u);
  EXPECT_LE(request.ownedBytesForTest(), filterCount * perFilterCap);
  auto* first = request.get(FilterKey("owned-memory-0"));
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, first->probe(0).kind());
  EXPECT_LE(request.ownedBytesForTest(), filterCount * perFilterCap);
}

TEST(FilterCacheTest, exactScoredSparseFilterUsesCachedDocSetLead) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  constexpr int32_t maxDoc = 1024;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    addTermDoc(writer, doc == 0 ? "alpha beta selected" : "alpha beta");
  }
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto cache = writer.snapshots.readers.filterCache;

  TermQuery alpha("text_w", "alpha");
  TermQuery beta("text_w", "beta");
  TermQuery selected("text_w", "selected");
  std::array<Query*, 2> required{&alpha, &beta};
  std::array<Query*, 1> filters{&selected};
  BooleanQuery query(required, {}, {}, filters);

  auto docSetLeads = [&](int32_t flags, bool disableExactPolicy) {
    ExactFilterCachePolicyGuard guard(disableExactPolicy);
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = query.createWeight(context, flags);
    MemPool execPool;
    auto* scorer = weight->createScorer(execPool, reader->segments()[0]);
    EXPECT_NE(nullptr, scorer);
    if (scorer == nullptr) return false;
    auto clauses = scorer->flatConjunctionScorers();
    EXPECT_EQ(3u, clauses.size());
    return !clauses.empty()
        && dynamic_cast<QueryPrep::DocSetScorer*>(clauses[0]) != nullptr;
  };

  auto initial = cache->counters();
  EXPECT_FALSE(docSetLeads(Query::NEED_SCORES, true));
  EXPECT_FALSE(docSetLeads(
      Query::NEED_SCORES | Query::ALLOW_PRUNING, false));
  auto bypassed = cache->counters();
  EXPECT_EQ(initial.builds, bypassed.builds);
  EXPECT_EQ(initial.hits, bypassed.hits);

  EXPECT_TRUE(docSetLeads(Query::NEED_SCORES, false));
  auto built = cache->counters();
  EXPECT_EQ(initial.builds + 1, built.builds);
  EXPECT_TRUE(docSetLeads(Query::NEED_SCORES, false));
  EXPECT_EQ(built.hits + 1, cache->counters().hits);

  EXPECT_TRUE(docSetLeads(0, false));
  EXPECT_TRUE(docSetLeads(0, true));
}

TEST(FilterCacheIntegrationTest,
     scoredSparseFilterCandidateFeedsMatchOracles) {
  FilteredConjunctionBatchSizeGuard batchSizeGuard(
      Postings::DOCS_BLOCK_SIZE);
  LuxirConfig cachedConfig;
  cachedConfig.queryCacheBytes = 4 * 1024 * 1024;
  LuxirConfig uncachedConfig;
  uncachedConfig.queryCacheBytes = 0;
  LuxirNode cachedNode(cachedConfig);
  LuxirNode uncachedNode(uncachedConfig);
  constexpr std::string_view collection =
      "exact_scored_sparse_filter_postings";
  luxir::test::CollectionHelper cached(cachedNode, collection);
  luxir::test::CollectionHelper uncached(uncachedNode, collection);

  constexpr int32_t maxDoc = 12 * DocsEnumMeta::L1_DOCS + 257;
  std::vector<luxir::test::Doc> docs;
  docs.reserve((size_t) maxDoc);
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    std::string body = "alpha beta";
    for (int32_t repeat = 0; repeat < doc % 5; repeat++) {
      body += " alpha";
    }
    for (int32_t repeat = 0; repeat < doc % 3; repeat++) {
      body += " beta";
    }
    if (doc % 3 == 0) {
      body += " gamma";
    }
    if (doc % 7 == 0) {
      body += " delta";
    }
    if (doc >= maxDoc / 2 && doc % 11 == 0) {
      body += " segment_only";
    }
    if (doc % 64 == 0) {
      body += " rare";
    }
    std::string filterValue = doc % 40 == 0
        ? "selected semidense"
        : doc % 20 == 0 ? "semidense" : "other";
    docs.push_back(luxir::test::flatdoc(
        "id", std::to_string(doc), "body_w", body,
        "filter_w", filterValue));
  }
  size_t split = docs.size() / 2;
  ASSERT_TRUE(cached.indexAll(
      std::span<const luxir::test::Doc>(docs).first(split),
      UpdateMessage::COMMIT).success);
  ASSERT_TRUE(cached.indexAll(
      std::span<const luxir::test::Doc>(docs).subspan(split),
      UpdateMessage::COMMIT).success);
  ASSERT_TRUE(uncached.indexAll(
      std::span<const luxir::test::Doc>(docs).first(split),
      UpdateMessage::COMMIT).success);
  ASSERT_TRUE(uncached.indexAll(
      std::span<const luxir::test::Doc>(docs).subspan(split),
      UpdateMessage::COMMIT).success);

  struct Result {
    int64_t count = 0;
    std::vector<std::string> ids;
    std::vector<float> scores;
    int64_t batchEngagements = 0;
    int64_t multiTermEngagements = 0;
    int64_t postingsFeedEngagements = 0;
    int64_t termFeedEngagements = 0;
  };
  enum class QueryShape {
    ONE_TERM,
    FILTER_LED,
    TERM_LED_MULTI,
    TERM_LED_ONE,
    FILTER_LED_NEGATED_ONE,
    FILTER_LED_NEGATED_TWO,
    FILTER_LED_NEGATED_GROUP,
    TERM_LED_NEGATED,
    SEGMENT_LOCAL_NEGATION,
    PHRASE_NEGATION,
  };
  auto run = [&](LuxirNode& node, std::string_view filterTerm,
                 bool pruning, bool disablePostingsFeed,
                 bool disableBatch = false,
                 QueryShape shape = QueryShape::ONE_TERM) {
    FilteredConjunctionPostingsFeedGuard feedGuard(
        disablePostingsFeed);
    FilteredConjunctionBatchGuard batchGuard(disableBatch);
    OwnedFilterStatsGuard statsGuard;
    auto request = luxir::test::localReq(node.getSearchEngine());
    auto& cursor = request->collection(collection)
        .topDocs("q")
        .matchFilter("filter_w", filterTerm)
        .fields({"id"})
        .limit(100);
    if (shape == QueryShape::TERM_LED_ONE) {
      cursor.matchQuery("body_w", "rare");
    } else if (shape == QueryShape::FILTER_LED_NEGATED_ONE
               || shape == QueryShape::FILTER_LED_NEGATED_TWO
               || shape == QueryShape::FILTER_LED_NEGATED_GROUP
               || shape == QueryShape::TERM_LED_NEGATED
               || shape == QueryShape::SEGMENT_LOCAL_NEGATION
               || shape == QueryShape::PHRASE_NEGATION) {
      auto& mr = cursor.mr();
      std::vector<api::Query> required;
      required.push_back(luxir::test::qb::match(
          mr, "body_w",
          shape == QueryShape::TERM_LED_NEGATED ? "rare" : "alpha"));
      std::vector<api::Query> prohibited;
      if (shape == QueryShape::FILTER_LED_NEGATED_GROUP) {
        std::array group = {
            luxir::test::qb::match(mr, "body_w", "gamma"),
            luxir::test::qb::match(mr, "body_w", "delta")};
        prohibited.push_back(
            luxir::test::qb::boolean(mr, {}, group));
      } else if (shape == QueryShape::SEGMENT_LOCAL_NEGATION) {
        prohibited.push_back(luxir::test::qb::match(
            mr, "body_w", "segment_only"));
      } else if (shape == QueryShape::PHRASE_NEGATION) {
        prohibited.push_back(luxir::test::qb::phraseWords(
            mr, "body_w", {"beta", "gamma"}));
      } else {
        prohibited.push_back(luxir::test::qb::match(
            mr, "body_w", "gamma"));
        if (shape == QueryShape::FILTER_LED_NEGATED_TWO) {
          prohibited.push_back(luxir::test::qb::match(
              mr, "body_w", "delta"));
        }
      }
      cursor.rawQuery() = luxir::test::qb::boolean(
          mr, required, {}, prohibited);
    } else if (shape != QueryShape::ONE_TERM) {
      std::string_view third = shape == QueryShape::FILTER_LED
          ? "gamma" : "rare";
      cursor.rawQuery() = luxir::test::qb::boolean(
          cursor.mr(),
          {luxir::test::qb::match(cursor.mr(), "body_w", "alpha"),
           luxir::test::qb::match(cursor.mr(), "body_w", "beta"),
           luxir::test::qb::match(cursor.mr(), "body_w", third)});
    } else {
      cursor.matchQuery("body_w", "alpha");
    }
    if (pruning) {
      cursor.getScores();
    } else {
      cursor.withStats();
    }
    request->execute(false);
    EXPECT_TRUE(request->ok()) << request->errorMsg();

    Result result;
    result.count = request->getMatchCount("q");
    const auto* list = request->docList("q");
    if (list != nullptr) {
      const auto* idColumn = list->columns.find("id");
      const auto* scoreColumn = list->columns.find("_score_");
      if (idColumn != nullptr && scoreColumn != nullptr) {
        const auto* ids = std::get_if<api::ColStr>(&idColumn->kind);
        const auto* scores =
            std::get_if<api::ColFloat>(&scoreColumn->kind);
        if (ids != nullptr && scores != nullptr) {
          for (auto id : ids->v) {
            result.ids.emplace_back(id);
          }
          result.scores.assign(scores->v.begin(), scores->v.end());
        }
      }
    }
    result.batchEngagements =
        SkipStats::filteredConjBatchEngagements;
    result.multiTermEngagements =
        SkipStats::filteredConjBatchMultiTermEngagements;
    result.postingsFeedEngagements =
        SkipStats::filteredConjBatchPostingsFeedEngagements;
    result.termFeedEngagements =
        SkipStats::candidateTermFeedEngagements;
    return result;
  };

  run(cachedNode, "selected", false, false);
  run(cachedNode, "selected", false, false);
  Result cachedResult = run(cachedNode, "selected", false, false);
  Result postingsResult = run(uncachedNode, "selected", false, false);
  Result disabledResult = run(uncachedNode, "selected", false, true);

  EXPECT_EQ((maxDoc + 39) / 40, cachedResult.count);
  EXPECT_EQ(100u, cachedResult.ids.size());
  EXPECT_EQ(cachedResult.ids.size(), cachedResult.scores.size());
  EXPECT_GT(cachedResult.batchEngagements, 0);
  EXPECT_EQ(0, cachedResult.postingsFeedEngagements);
  EXPECT_GT(postingsResult.postingsFeedEngagements, 0);
  EXPECT_EQ(cachedResult.count, postingsResult.count);
  EXPECT_EQ(cachedResult.ids, postingsResult.ids);
  EXPECT_EQ(cachedResult.scores, postingsResult.scores);
  EXPECT_EQ(0, disabledResult.batchEngagements);
  EXPECT_EQ(0, disabledResult.postingsFeedEngagements);

  // At density 1/20, TOP_100 keeps competitive pruning (the sparse reroute
  // cuts over at 1/32). The same candidate primitive must therefore let the
  // zero-score filter lead, probe and score the required term tails in cost
  // order, and preserve the window-mask oracle's ranked output for both filter
  // provenances.
  run(cachedNode, "semidense", true, false, false,
      QueryShape::FILTER_LED);
  run(cachedNode, "semidense", true, false, false,
      QueryShape::FILTER_LED);
  Result cachedPruned = run(
      cachedNode, "semidense", true, false, false,
      QueryShape::FILTER_LED);
  Result postingsPruned = run(
      uncachedNode, "semidense", true, false, false,
      QueryShape::FILTER_LED);
  Result maskPruned = run(
      uncachedNode, "semidense", true, false, true,
      QueryShape::FILTER_LED);

  EXPECT_EQ(100u, cachedPruned.ids.size());
  EXPECT_GT(cachedPruned.batchEngagements, 0);
  EXPECT_GT(cachedPruned.multiTermEngagements, 0);
  EXPECT_EQ(0, cachedPruned.postingsFeedEngagements);
  EXPECT_GT(postingsPruned.batchEngagements, 0);
  EXPECT_GT(postingsPruned.multiTermEngagements, 0);
  EXPECT_GT(postingsPruned.postingsFeedEngagements, 0);
  EXPECT_EQ(0, maskPruned.batchEngagements);
  EXPECT_EQ(0, maskPruned.multiTermEngagements);
  EXPECT_EQ(0, maskPruned.postingsFeedEngagements);
  EXPECT_EQ(cachedPruned.ids, postingsPruned.ids);
  EXPECT_EQ(cachedPruned.scores, postingsPruned.scores);
  EXPECT_EQ(maskPruned.ids, postingsPruned.ids);
  EXPECT_EQ(maskPruned.scores, postingsPruned.scores);

  // A rare scoring term reverses ownership: its docs-only postings feed is
  // cheaper than the filter, while an independent cursor scores the lead
  // survivors and the original scoring cursors retain whole-window bounds.
  auto verifyTermLead = [&](QueryShape shape, bool multiTerm) {
    run(cachedNode, "semidense", true, false, false, shape);
    run(cachedNode, "semidense", true, false, false, shape);
    Result cachedTermLead = run(
        cachedNode, "semidense", true, false, false, shape);
    Result postingsTermLead = run(
        uncachedNode, "semidense", true, false, false, shape);
    Result maskTermLead = run(
        uncachedNode, "semidense", true, false, true, shape);

    EXPECT_EQ(100u, cachedTermLead.ids.size());
    EXPECT_GT(cachedTermLead.batchEngagements, 0);
    EXPECT_EQ(multiTerm, cachedTermLead.multiTermEngagements > 0);
    EXPECT_GT(cachedTermLead.termFeedEngagements, 0);
    EXPECT_EQ(0, cachedTermLead.postingsFeedEngagements);
    EXPECT_GT(postingsTermLead.batchEngagements, 0);
    EXPECT_EQ(multiTerm, postingsTermLead.multiTermEngagements > 0);
    EXPECT_GT(postingsTermLead.termFeedEngagements, 0);
    EXPECT_EQ(0, postingsTermLead.postingsFeedEngagements);
    EXPECT_EQ(0, maskTermLead.batchEngagements);
    EXPECT_EQ(0, maskTermLead.multiTermEngagements);
    EXPECT_EQ(0, maskTermLead.termFeedEngagements);
    EXPECT_EQ(cachedTermLead.ids, postingsTermLead.ids);
    EXPECT_EQ(cachedTermLead.scores, postingsTermLead.scores);
    EXPECT_EQ(maskTermLead.ids, postingsTermLead.ids);
    EXPECT_EQ(maskTermLead.scores, postingsTermLead.scores);
  };
  verifyTermLead(QueryShape::TERM_LED_MULTI, true);
  verifyTermLead(QueryShape::TERM_LED_ONE, false);

  // Direct term exclusions consume the already-small candidate batch as an
  // OR membership mask. They do not participate in candidate ownership,
  // scoring, or score bounds. Exercise both filter provenances, both lead
  // owners, separate and grouped exclusions, and a segment where the
  // exclusion has no scorer.
  auto verifyNegated = [&](QueryShape shape, bool termLead) {
    run(cachedNode, "semidense", true, false, false, shape);
    run(cachedNode, "semidense", true, false, false, shape);
    Result cachedNegated = run(
        cachedNode, "semidense", true, false, false, shape);
    Result postingsNegated = run(
        uncachedNode, "semidense", true, false, false, shape);
    Result pullNegated = run(
        uncachedNode, "semidense", true, false, true, shape);

    EXPECT_FALSE(cachedNegated.ids.empty());
    EXPECT_GT(cachedNegated.batchEngagements, 0);
    EXPECT_EQ(termLead, cachedNegated.termFeedEngagements > 0);
    EXPECT_EQ(0, cachedNegated.postingsFeedEngagements);
    EXPECT_GT(postingsNegated.batchEngagements, 0);
    EXPECT_EQ(termLead, postingsNegated.termFeedEngagements > 0);
    EXPECT_EQ(!termLead,
              postingsNegated.postingsFeedEngagements > 0);
    EXPECT_EQ(0, pullNegated.batchEngagements);
    EXPECT_EQ(cachedNegated.count, postingsNegated.count);
    EXPECT_EQ(pullNegated.count, postingsNegated.count);
    EXPECT_EQ(cachedNegated.ids, postingsNegated.ids);
    EXPECT_EQ(cachedNegated.scores, postingsNegated.scores);
    EXPECT_EQ(pullNegated.ids, postingsNegated.ids);
    EXPECT_EQ(pullNegated.scores, postingsNegated.scores);
  };
  verifyNegated(QueryShape::FILTER_LED_NEGATED_ONE, false);
  verifyNegated(QueryShape::FILTER_LED_NEGATED_TWO, false);
  verifyNegated(QueryShape::FILTER_LED_NEGATED_GROUP, false);
  verifyNegated(QueryShape::TERM_LED_NEGATED, true);
  verifyNegated(QueryShape::SEGMENT_LOCAL_NEGATION, false);

  // A two-phase exclusion cannot be represented by the term-membership
  // primitive and must preserve the pull plan.
  Result phrase = run(
      uncachedNode, "semidense", true, false, false,
      QueryShape::PHRASE_NEGATION);
  Result phrasePull = run(
      uncachedNode, "semidense", true, false, true,
      QueryShape::PHRASE_NEGATION);
  EXPECT_EQ(0, phrase.batchEngagements);
  EXPECT_EQ(phrasePull.ids, phrase.ids);
  EXPECT_EQ(phrasePull.scores, phrase.scores);
}

enum class ExactFilteredConjShape {
  THREE_TERMS,
  RARE_TERM,
  PHRASE,
  TWO_FILTERS,
  RARE_TERM_TWO_FILTERS,
};

struct ExactFilteredConjResult {
  int64_t count = 0;
  std::vector<std::string> ids;
  std::vector<float> scores;
  int64_t batchEngagements = 0;
  int64_t multiTermEngagements = 0;
  int64_t postingsFeedEngagements = 0;
  int64_t termFeedEngagements = 0;
  int64_t scoredProbeAdvances = 0;
  int64_t scoreWindows = 0;
};

bool indexExactFilteredConjDocs(LuxirNode& node,
                                std::string_view collection) {
  constexpr int32_t maxDoc = 2000;
  luxir::test::CollectionHelper helper(node, collection);
  std::vector<luxir::test::Doc> docs;
  docs.reserve((size_t) maxDoc);
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    // gamma is absent from 2/3 of docs so the batch's tail probes reject
    // (and compact away) most filter-led candidates instead of all matching.
    std::string body = "alpha beta quick fox";
    for (int32_t repeat = 0; repeat < doc % 5; repeat++) {
      body += " alpha";
    }
    for (int32_t repeat = 0; repeat < doc % 3; repeat++) {
      body += " beta";
    }
    if (doc % 3 == 0) {
      for (int32_t repeat = 0; repeat <= doc % 2; repeat++) {
        body += " gamma";
      }
    }
    if (doc % 100 == 0) {
      body += " rare";
    }
    docs.push_back(luxir::test::flatdoc(
        "id", std::to_string(doc), "body_w", body,
        "filter_w", doc % 40 == 0 ? "selected" : "other",
        "second_filter_w",
        doc % 10 == 0 && doc % 400 != 200 ? "wide" : "other"));
  }
  return helper.indexAll(docs, UpdateMessage::COMMIT).success;
}

ExactFilteredConjResult runExactFilteredConj(
    LuxirNode& node, std::string_view collection,
    ExactFilteredConjShape shape, bool disableMultiTerm,
    bool disableBatch = false, bool disableTermFeed = false) {
  FilteredConjMultiTermGuard multiTermGuard(disableMultiTerm);
  FilteredConjunctionBatchGuard batchGuard(disableBatch);
  CandidateTermFeedGuard termFeedGuard(disableTermFeed);
  OwnedFilterStatsGuard statsGuard;
  auto request = luxir::test::localReq(node.getSearchEngine());
  request->collection(collection);
  auto& cursor = request->topDocs("q");
  cursor.countLimit().withStats().fields({"id"}).limit(100);
  switch (shape) {
    case ExactFilteredConjShape::THREE_TERMS:
      cursor.rawQuery() = luxir::test::qb::boolean(
          cursor.mr(),
          {luxir::test::qb::match(cursor.mr(), "body_w", "alpha"),
           luxir::test::qb::match(cursor.mr(), "body_w", "beta"),
           luxir::test::qb::match(cursor.mr(), "body_w", "gamma")});
      cursor.matchFilter("filter_w", "selected");
      break;
    case ExactFilteredConjShape::RARE_TERM:
      cursor.rawQuery() = luxir::test::qb::boolean(
          cursor.mr(),
          {luxir::test::qb::match(cursor.mr(), "body_w", "alpha"),
           luxir::test::qb::match(cursor.mr(), "body_w", "rare"),
           luxir::test::qb::match(cursor.mr(), "body_w", "gamma")});
      // The wide filter keeps the feed (rare, df 20) under the DocSet
      // provenance ratio gate; "selected" (df 50) sits inside it.
      cursor.matchFilter("second_filter_w", "wide");
      break;
    case ExactFilteredConjShape::PHRASE:
      cursor.rawQuery() = luxir::test::qb::phraseWords(
          cursor.mr(), "body_w", {"quick", "fox"});
      cursor.matchFilter("filter_w", "selected");
      break;
    case ExactFilteredConjShape::TWO_FILTERS:
      cursor.rawQuery() = luxir::test::qb::boolean(
          cursor.mr(),
          {luxir::test::qb::match(cursor.mr(), "body_w", "alpha"),
           luxir::test::qb::match(cursor.mr(), "body_w", "beta")});
      cursor.matchFilter("filter_w", "selected");
      cursor.matchFilter("second_filter_w", "wide");
      break;
    case ExactFilteredConjShape::RARE_TERM_TWO_FILTERS:
      cursor.rawQuery() = luxir::test::qb::boolean(
          cursor.mr(),
          {luxir::test::qb::match(cursor.mr(), "body_w", "alpha"),
           luxir::test::qb::match(cursor.mr(), "body_w", "rare"),
           luxir::test::qb::match(cursor.mr(), "body_w", "gamma")});
      cursor.matchFilter("filter_w", "selected");
      cursor.matchFilter("second_filter_w", "wide");
      break;
  }
  request->execute(false);
  EXPECT_TRUE(request->ok()) << request->errorMsg();

  ExactFilteredConjResult result;
  if (!request->ok()) {
    return result;
  }
  result.count = request->getMatchCount("q");
  const auto* list = request->docList("q");
  if (list != nullptr) {
    const auto* idColumn = list->columns.find("id");
    const auto* scoreColumn = list->columns.find("_score_");
    EXPECT_NE(nullptr, idColumn);
    EXPECT_NE(nullptr, scoreColumn);
    if (idColumn != nullptr && scoreColumn != nullptr) {
      const auto* ids = std::get_if<api::ColStr>(&idColumn->kind);
      const auto* scores = std::get_if<api::ColFloat>(&scoreColumn->kind);
      EXPECT_NE(nullptr, ids);
      EXPECT_NE(nullptr, scores);
      if (ids != nullptr && scores != nullptr) {
        for (auto id : ids->v) {
          result.ids.emplace_back(id);
        }
        result.scores.assign(scores->v.begin(), scores->v.end());
      }
    }
  }
  result.batchEngagements = SkipStats::filteredConjBatchEngagements;
  result.multiTermEngagements =
      SkipStats::filteredConjBatchMultiTermEngagements;
  result.postingsFeedEngagements =
      SkipStats::filteredConjBatchPostingsFeedEngagements;
  result.termFeedEngagements =
      SkipStats::candidateTermFeedEngagements;
  result.scoredProbeAdvances = SkipStats::scoredProbeAdvances;
  result.scoreWindows = SkipStats::filteredConjBatchScoreWindows;
  return result;
}

void expectSameExactFilteredConj(const ExactFilteredConjResult& expected,
                                 const ExactFilteredConjResult& actual) {
  EXPECT_EQ(expected.count, actual.count);
  EXPECT_EQ(expected.ids, actual.ids);
  EXPECT_EQ(expected.scores, actual.scores);
}

TEST(FilterCacheIntegrationTest,
     filteredConjMultiTermMatchesCachedAndPostingsFilters) {
  LuxirConfig cachedConfig;
  cachedConfig.queryCacheBytes = 4 * 1024 * 1024;
  LuxirConfig uncachedConfig;
  uncachedConfig.queryCacheBytes = 0;
  LuxirNode cachedNode(cachedConfig);
  LuxirNode uncachedNode(uncachedConfig);
  constexpr std::string_view collection =
      "filtered_conj_multi_term_provenance";
  ASSERT_TRUE(indexExactFilteredConjDocs(cachedNode, collection));
  ASSERT_TRUE(indexExactFilteredConjDocs(uncachedNode, collection));

  runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::THREE_TERMS, false);
  runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::THREE_TERMS, false);
  ExactFilteredConjResult cached = runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::THREE_TERMS, false);
  ExactFilteredConjResult cachedBaseline = runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::THREE_TERMS, true);
  ExactFilteredConjResult postings = runExactFilteredConj(
      uncachedNode, collection, ExactFilteredConjShape::THREE_TERMS, false);
  ExactFilteredConjResult postingsBaseline = runExactFilteredConj(
      uncachedNode, collection, ExactFilteredConjShape::THREE_TERMS, true);

  EXPECT_EQ(17, cached.count);
  expectSameExactFilteredConj(cachedBaseline, cached);
  expectSameExactFilteredConj(postingsBaseline, postings);
  EXPECT_GT(cached.multiTermEngagements, 0);
  EXPECT_EQ(0, cached.postingsFeedEngagements);
  EXPECT_GT(postings.multiTermEngagements, 0);
  EXPECT_GT(postings.postingsFeedEngagements, 0);
  EXPECT_EQ(0, cachedBaseline.multiTermEngagements);
  EXPECT_EQ(0, postingsBaseline.multiTermEngagements);
}

TEST(FilterCacheIntegrationTest, filteredConjMultiTermRatioGateDeclines) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filtered_conj_ratio_gate";
  ASSERT_TRUE(indexExactFilteredConjDocs(node, collection));

  ExactFilteredConjResult engaged = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::THREE_TERMS, false);
  ExactFilteredConjResult declined;
  {
    FilteredConjRatioGuard ratioGuard(1000000);
    declined = runExactFilteredConj(
        node, collection, ExactFilteredConjShape::THREE_TERMS, false);
  }

  expectSameExactFilteredConj(engaged, declined);
  EXPECT_GT(engaged.multiTermEngagements, 0);
  EXPECT_EQ(0, declined.batchEngagements);
  EXPECT_EQ(0, declined.multiTermEngagements);
}

TEST(FilterCacheIntegrationTest,
     candidateTermFeedMatchesKillSwitchForBothFilterProvenances) {
  LuxirConfig cachedConfig;
  cachedConfig.queryCacheBytes = 4 * 1024 * 1024;
  LuxirConfig uncachedConfig;
  uncachedConfig.queryCacheBytes = 0;
  LuxirNode cachedNode(cachedConfig);
  LuxirNode uncachedNode(uncachedConfig);
  constexpr std::string_view collection = "exact_candidate_term_feed";
  ASSERT_TRUE(indexExactFilteredConjDocs(cachedNode, collection));
  ASSERT_TRUE(indexExactFilteredConjDocs(uncachedNode, collection));

  runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
  runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
  ExactFilteredConjResult cached = runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
  ExactFilteredConjResult cachedKillSwitch = runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::RARE_TERM, false,
      false, true);
  ExactFilteredConjResult postings = runExactFilteredConj(
      uncachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
  ExactFilteredConjResult postingsKillSwitch = runExactFilteredConj(
      uncachedNode, collection, ExactFilteredConjShape::RARE_TERM, false,
      false, true);

  EXPECT_EQ(5, cached.count);
  expectSameExactFilteredConj(cachedKillSwitch, cached);
  expectSameExactFilteredConj(postingsKillSwitch, postings);
  expectSameExactFilteredConj(cached, postings);
  EXPECT_GT(cached.termFeedEngagements, 0);
  EXPECT_GT(postings.termFeedEngagements, 0);
  EXPECT_GT(cached.scoredProbeAdvances, 0);
  EXPECT_GT(postings.scoredProbeAdvances, 0);
  EXPECT_GT(cached.scoreWindows, 0);
  EXPECT_GT(postings.scoreWindows, 0);
  EXPECT_EQ(0, cachedKillSwitch.termFeedEngagements);
  EXPECT_EQ(0, postingsKillSwitch.termFeedEngagements);
}

TEST(FilterCacheIntegrationTest, filteredConjPhraseTailDeclines) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filtered_conj_phrase_tail";
  ASSERT_TRUE(indexExactFilteredConjDocs(node, collection));

  ExactFilteredConjResult enabled = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::PHRASE, false);
  ExactFilteredConjResult baseline = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::PHRASE, false, true);

  EXPECT_EQ(50, enabled.count);
  expectSameExactFilteredConj(baseline, enabled);
  EXPECT_EQ(0, enabled.batchEngagements);
}

TEST(FilterCacheIntegrationTest, candidateTermFeedHandlesTwoFilters) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "exact_candidate_two_filters";
  ASSERT_TRUE(indexExactFilteredConjDocs(node, collection));

  ExactFilteredConjResult enabled = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::RARE_TERM_TWO_FILTERS,
      false);
  ExactFilteredConjResult baseline = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::RARE_TERM_TWO_FILTERS,
      false, false, true);

  EXPECT_EQ(2, enabled.count);
  expectSameExactFilteredConj(baseline, enabled);
  EXPECT_GT(enabled.termFeedEngagements, 0);
  EXPECT_GT(enabled.scoredProbeAdvances, 0);
  EXPECT_EQ(0, baseline.termFeedEngagements);
}

TEST(FilterCacheIntegrationTest,
     exactCandidateFilterLeadKeepsExistingRouteAndCounters) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "exact_candidate_filter_lead";
  ASSERT_TRUE(indexExactFilteredConjDocs(node, collection));

  ExactFilteredConjResult enabled = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::TWO_FILTERS, false);
  ExactFilteredConjResult termFeedDisabled = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::TWO_FILTERS, false,
      false, true);

  EXPECT_EQ(45, enabled.count);
  expectSameExactFilteredConj(termFeedDisabled, enabled);
  EXPECT_GT(enabled.multiTermEngagements, 0);
  EXPECT_GT(enabled.postingsFeedEngagements, 0);
  EXPECT_EQ(0, enabled.termFeedEngagements);
  EXPECT_EQ(enabled.batchEngagements,
            termFeedDisabled.batchEngagements);
  EXPECT_EQ(enabled.multiTermEngagements,
            termFeedDisabled.multiTermEngagements);
  EXPECT_EQ(enabled.postingsFeedEngagements,
            termFeedDisabled.postingsFeedEngagements);
  EXPECT_EQ(enabled.scoreWindows, termFeedDisabled.scoreWindows);
  EXPECT_EQ(0, termFeedDisabled.termFeedEngagements);
}

TEST(FilterCacheIntegrationTest, candidateTermFeedDensityCapDeclines) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "exact_candidate_density_cap";
  ASSERT_TRUE(indexExactFilteredConjDocs(node, collection));

  ExactFilteredConjResult admitted = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::RARE_TERM, false);
  ExactFilteredConjResult declined;
  {
    CandidateTermFeedFractionGuard guard(1000000);
    declined = runExactFilteredConj(
        node, collection, ExactFilteredConjShape::RARE_TERM, false);
  }

  expectSameExactFilteredConj(admitted, declined);
  EXPECT_GT(admitted.termFeedEngagements, 0);
  EXPECT_EQ(0, declined.termFeedEngagements);
}

// The DocSet-provenance ratio gate declines a term feed whose cost is too
// close to the warm filter's; the raw-postings provenance carries no such
// gate and must keep engaging under the same override.
TEST(FilterCacheIntegrationTest, candidateTermFeedDocSetRatioDeclines) {
  LuxirConfig cachedConfig;
  cachedConfig.queryCacheBytes = 4 * 1024 * 1024;
  LuxirConfig uncachedConfig;
  uncachedConfig.queryCacheBytes = 0;
  LuxirNode cachedNode(cachedConfig);
  LuxirNode uncachedNode(uncachedConfig);
  constexpr std::string_view collection = "exact_candidate_docset_ratio";
  ASSERT_TRUE(indexExactFilteredConjDocs(cachedNode, collection));
  ASSERT_TRUE(indexExactFilteredConjDocs(uncachedNode, collection));

  runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
  ExactFilteredConjResult admitted = runExactFilteredConj(
      cachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
  ExactFilteredConjResult declined;
  ExactFilteredConjResult postings;
  {
    CandidateTermFeedDocSetRatioGuard guard(1000000);
    declined = runExactFilteredConj(
        cachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
    postings = runExactFilteredConj(
        uncachedNode, collection, ExactFilteredConjShape::RARE_TERM, false);
  }

  expectSameExactFilteredConj(admitted, declined);
  expectSameExactFilteredConj(admitted, postings);
  EXPECT_GT(admitted.termFeedEngagements, 0);
  EXPECT_EQ(0, declined.termFeedEngagements);
  EXPECT_GT(postings.termFeedEngagements, 0);
}

TEST(FilterCacheTest, effectiveMaterializationOffersRawByproduct) {
  FilterCacheConfig config = testConfig();
  config.minSegmentDocs = 0;
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "cache me");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto cache = writer.snapshots.readers.filterCache;
  TermQuery query("text_w", "cache");
  MemPool contextPool;
  Query::Context context(contextPool, *reader);
  auto* use = context.getFilterUse(query);
  auto heldClaim = use->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, heldClaim.kind());
  MemPool racerPool;
  Query::Context racerContext(racerPool, *reader);
  auto* racerWeight = query.createWeight(racerContext, 0);
  auto* racerUse = racerContext.getFilterUse(query);

  auto effective = QueryPrep::materializeEffectiveFilter(
      *racerWeight, racerUse, *reader, reader->segments()[0], nullptr);
  ASSERT_NE(nullptr, effective.get());
  EXPECT_EQ(1, effective.get()->card());
  EXPECT_EQ(1u, cache->counters().byproductInserts);
}

TEST(FilterCacheTest, preparedDomainProvenanceControlsQueryKeyPublication) {
  FilterCacheConfig config = testConfig();
  config.minSegmentDocs = 0;
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "a keep");
  addTermDoc(writer, "a");
  addTermDoc(writer, "keep");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();

  TermQuery term("text_w", "a");
  ForcePrepareQuery forcedTerm(&term);
  MemPool canonicalPool;
  Query::Context canonicalContext(canonicalPool, *reader);
  auto* canonicalWeight = forcedTerm.createWeight(canonicalContext, 0);
  std::array<Query::Weight*, 1> canonicalWeights{canonicalWeight};
  std::array<FilterCache::Use*, 1> canonicalUses{
      canonicalContext.getFilterUse(forcedTerm)};
  Query::Weight::PrepareContext canonicalPrepare{
      *reader, std::span<DocSet* const>{}, nullptr, nullptr};
  auto canonical = QueryPrep::prepareFilterSources(
      canonicalWeights, canonicalUses, canonicalPrepare);
  ASSERT_EQ(1u, canonical.size());
  EXPECT_FALSE(canonical[0].prepared->outputIsSubsetOfDomain());
  EXPECT_EQ(PreparedDomainDependence::QUERY_CANONICAL,
            canonical[0].domainDependence);
  auto canonicalDocs = QueryPrep::materializeEffectiveFilter(
      canonical[0], *reader, reader->segments()[0], nullptr);
  ASSERT_NE(nullptr, canonicalDocs.get());
  EXPECT_EQ(2, canonicalDocs.get()->card());
  auto canonicalProbe = canonical[0].cacheUse->probe(0);
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, canonicalProbe.kind());

  TermQuery keep("text_w", "keep");
  ForcePrepareQuery preparedMandatory(&term);
  std::array<Query*, 1> mandatory{&preparedMandatory};
  std::array<Query*, 1> filters{&keep};
  BooleanQuery domainDependent(mandatory, {}, {}, filters);
  MemPool dependentPool;
  Query::Context dependentContext(dependentPool, *reader);
  auto* dependentWeight = domainDependent.createWeight(dependentContext, 0);
  std::array<Query::Weight*, 1> dependentWeights{dependentWeight};
  std::array<FilterCache::Use*, 1> dependentUses{
      dependentContext.getFilterUse(domainDependent)};
  Query::Weight::PrepareContext dependentPrepare{
      *reader, std::span<DocSet* const>{}, nullptr, nullptr};
  auto dependent = QueryPrep::prepareFilterSources(
      dependentWeights, dependentUses, dependentPrepare);
  ASSERT_EQ(1u, dependent.size());
  EXPECT_TRUE(dependent[0].prepared->outputIsSubsetOfDomain());
  EXPECT_EQ(PreparedDomainDependence::PREPARE_DOMAIN,
            dependent[0].domainDependence);
  auto dependentDocs = QueryPrep::materializeEffectiveFilter(
      dependent[0], *reader, reader->segments()[0], nullptr);
  ASSERT_NE(nullptr, dependentDocs.get());
  EXPECT_EQ(1, dependentDocs.get()->card());
  auto dependentProbe = dependent[0].cacheUse->probe(0);
  EXPECT_NE(FilterCache::Probe::Kind::HIT, dependentProbe.kind());
}

TEST(FilterCacheTest, pruningBypassDoesNotOfferByproduct) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "cache me");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto cache = writer.snapshots.readers.filterCache;
  TermQuery query("text_w", "cache");
  MemPool contextPool;
  Query::Context context(contextPool, *reader);
  auto* use = context.getFilterUse(query);
  auto heldClaim = use->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, heldClaim.kind());
  MemPool racerPool;
  Query::Context racerContext(racerPool, *reader);
  auto* racerWeight =
      query.createWeight(racerContext, Query::ALLOW_PRUNING);
  auto* racerUse = racerContext.getFilterUse(query);

  auto effective = QueryPrep::materializeEffectiveFilter(
      *racerWeight, racerUse, *reader, reader->segments()[0], nullptr);

  ASSERT_NE(nullptr, effective.get());
  EXPECT_EQ(1, effective.get()->card());
  EXPECT_EQ(0u, cache->counters().byproductInserts);
}

TEST(FilterCacheTest, domainCompositionsAreOwnedAndLiveCompositionIsMemoized) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "cache");
  addTermDoc(writer, "cache");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto cache = writer.snapshots.readers.filterCache;
  std::array identities{FilterCache::SegmentIdentity{
      reader->segments()[0].segInfo.seg_id, reader->segments()[0].maxDoc()}};
  FilterCache::UseRegistry request(*cache, reader->coreGen(), identities);
  auto* use = request.get(FilterKey("domain-composition"));
  use->offerRaw(0, docs(3, {0, 1}), 1);
  auto domain = docs(3, {1, 2});
  auto otherDomain = docs(3, {0, 2});

  DomainHandle domainEffective = use->effectiveDomain(
      0, *reader, domain.get());
  DomainHandle otherEffective = use->effectiveDomain(
      0, *reader, otherDomain.get());
  DomainHandle domainAgain = use->effectiveDomain(
      0, *reader, domain.get());
  DocSet* liveEffective = use->effectiveDocSet(0, *reader);
  DomainHandle liveAgain = use->effectiveDomain(0, *reader, nullptr);

  ASSERT_NE(nullptr, domainEffective.get());
  EXPECT_EQ(1, domainEffective.get()->card());
  EXPECT_TRUE(domainEffective.get()->get(1));
  ASSERT_NE(nullptr, otherEffective.get());
  EXPECT_EQ(1, otherEffective.get()->card());
  EXPECT_TRUE(otherEffective.get()->get(0));
  // A domain composition is owned by the caller and never memoized: the Use
  // retains nothing about the domain, so a later short-lived domain at the
  // same address cannot be served an earlier composition.
  EXPECT_TRUE(domainEffective.isDeliverable());
  ASSERT_NE(nullptr, domainAgain.get());
  EXPECT_NE(domainEffective.get(), domainAgain.get());
  EXPECT_EQ(1, domainAgain.get()->card());
  // The request-stable value is memoized and borrowed.
  ASSERT_NE(nullptr, liveEffective);
  EXPECT_EQ(2, liveEffective->card());
  EXPECT_EQ(liveEffective, liveAgain.get());
  EXPECT_FALSE(liveAgain.isDeliverable());

  // An owned composition outlives both inputs: the domain it was composed
  // with and the Use that composed it.
  DomainHandle survivor;
  {
    FilterCache::UseRegistry scoped(*cache, reader->coreGen(), identities);
    auto* scopedUse = scoped.get(FilterKey("domain-composition-scoped"));
    scopedUse->offerRaw(0, docs(3, {0, 1}), 1);
    auto transient = docs(3, {1, 2});
    survivor = scopedUse->effectiveDomain(0, *reader, transient.get());
  }
  ASSERT_NE(nullptr, survivor.get());
  EXPECT_TRUE(survivor.isDeliverable());
  EXPECT_EQ(1, survivor.get()->card());
  EXPECT_TRUE(survivor.get()->get(1));
}

TEST(FilterCacheTest, rawMaterializationEnforcesExhaustiveFlags) {
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
  IndexWriter writer(writerSnapshots);
  addTermDoc(writer, "cache me");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  TermQuery query("text_w", "cache");
  MemPool contextPool;
  Query::Context context(contextPool, *reader);

  auto* scored = query.createWeight(context, Query::NEED_SCORES);
  EXPECT_THROW(QueryPrep::materializeRawFilter(
      *scored, nullptr, reader->segments()[0]), std::logic_error);
  auto* pruning = query.createWeight(context, Query::ALLOW_PRUNING);
  EXPECT_THROW(QueryPrep::materializeRawFilter(
      *pruning, nullptr, reader->segments()[0]), std::logic_error);
}

TEST(FilterCacheTest, recursiveRawBuildUsesRawHitsAcrossBooleanFilterModes) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addIdTermDoc(writer, "0", "body selected alpha");
  addIdTermDoc(writer, "1", "body selected beta");
  addIdTermDoc(writer, "2", "body alpha");
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("0", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = writer.snapshots.readers.getReader(0);
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  ASSERT_NE(nullptr, segment.liveDocs());

  TermQuery selected("text_w", "selected");
  {
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = selected.createWeight(context, 0);
    auto* use = context.getFilterUse(selected);
    MemPool execPool;
    ASSERT_NE(nullptr, QueryPrep::filterSupplier(
        execPool, *weight, use, *reader, segment,
        QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE));
    ASSERT_NE(nullptr, use->rawDocSet(0));
    EXPECT_EQ(2, use->rawDocSet(0)->card());
    EXPECT_EQ(1, use->effectiveDocSet(0, *reader)->card());
  }

  auto assertRawBuild = [&](Query& query) {
    auto before = writer.snapshots.readers.filterCache->counters();
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query);
    auto effective = QueryPrep::materializeEffectiveFilter(
        *weight, use, *reader, segment, nullptr);
    ASSERT_NE(nullptr, effective.get());
    ASSERT_NE(nullptr, use->rawDocSet(0));
    EXPECT_EQ(2, use->rawDocSet(0)->card());
    EXPECT_TRUE(use->rawDocSet(0)->get(0));
    EXPECT_EQ(1, effective.get()->card());
    EXPECT_FALSE(effective.get()->get(0));
    auto after = writer.snapshots.readers.filterCache->counters();
    EXPECT_GT(after.hits, before.hits);
    EXPECT_EQ(before.builds + 1, after.builds);
  };

  TermQuery body("text_w", "body");
  std::array<Query*, 1> mandatory{&body};
  std::array<Query*, 1> filters{&selected};
  BooleanQuery exhaustive(mandatory, {}, {}, filters);
  assertRawBuild(exhaustive);

  TermQuery alpha("text_w", "alpha");
  TermQuery beta("text_w", "beta");
  std::array<Query*, 2> optional{&alpha, &beta};
  BooleanQuery sparseBatch({}, optional, {}, filters, 1);
  assertRawBuild(sparseBatch);
}

TEST(FilterCacheTest, recursiveRawBuildUsesRawProhibitedHit) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addIdTermDoc(writer, "0", "body selected");
  addIdTermDoc(writer, "1", "body selected");
  addIdTermDoc(writer, "2", "body");
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("0", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = writer.snapshots.readers.getReader(0);
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  ASSERT_NE(nullptr, segment.liveDocs());

  TermQuery body("text_w", "body");
  TermQuery selected("text_w", "selected");
  std::array<Query*, 1> mandatory{&body};
  std::array<Query*, 1> prohibited{&selected};
  BooleanQuery outer(mandatory, {}, prohibited, {});

  {
    MemPool warmPool;
    Query::Context warmContext(warmPool, *reader);
    auto* warmWeight = selected.createWeight(warmContext, 0);
    auto* warmUse = warmContext.getFilterUse(selected);
    MemPool execPool;
    ASSERT_NE(nullptr, QueryPrep::filterSupplier(
        execPool, *warmWeight, warmUse, *reader, segment,
        QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE));
    ASSERT_NE(nullptr, warmUse->rawDocSet(0));
    EXPECT_EQ(2, warmUse->rawDocSet(0)->card());
  }

  auto hitsBefore = writer.snapshots.readers.filterCache->counters().hits;
  MemPool contextPool;
  Query::Context context(contextPool, *reader);
  auto* weight = outer.createWeight(context, 0);
  auto* outerUse = context.getFilterUse(outer);
  auto* prohibitedUse = context.getFilterUse(selected);
  auto effective = QueryPrep::materializeEffectiveFilter(
      *weight, outerUse, *reader, segment, nullptr);

  ASSERT_NE(nullptr, prohibitedUse->rawDocSet(0));
  EXPECT_EQ(2, prohibitedUse->rawDocSet(0)->card());
  EXPECT_TRUE(prohibitedUse->rawDocSet(0)->get(0));
  EXPECT_EQ(1, prohibitedUse->effectiveDocSet(0, *reader)->card());
  ASSERT_NE(nullptr, outerUse->rawDocSet(0));
  EXPECT_EQ(1, outerUse->rawDocSet(0)->card());
  EXPECT_TRUE(outerUse->rawDocSet(0)->get(2));
  ASSERT_NE(nullptr, effective.get());
  EXPECT_EQ(1, effective.get()->card());
  EXPECT_TRUE(effective.get()->get(2));
  EXPECT_GT(writer.snapshots.readers.filterCache->counters().hits, hitsBefore);
}

TEST(FilterCacheTest, preparedProhibitedSourceBuildsThenHits) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "body selected");
  addTermDoc(writer, "body");
  addTermDoc(writer, "body selected");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader(0);
  auto& segment = reader->segments()[0];

  auto run = [&](bool prepareMandatory) {
    MemPool pool;
    Query::Context context(pool, *reader);
    TermQuery body("text_w", "body");
    TermQuery selected("text_w", "selected");
    ForcePrepareQuery preparedSelected(&selected);
    ForcePrepareQuery preparedBody(&body);
    std::array<Query*, 1> mandatory{prepareMandatory ? (Query*)&preparedBody : &body};
    std::array<Query*, 1> prohibited{&preparedSelected};
    BooleanQuery query(mandatory, {}, prohibited, {});
    auto* weight = query.createWeight(context, 0);
    EXPECT_TRUE(weight->needsPrepare());
    EXPECT_FALSE(weight->consumesDomain());
    Query::Weight::PrepareContext prepareContext{
        *reader, std::span<DocSet* const>{}, nullptr, nullptr};
    auto prepared = weight->prepare(prepareContext);
    if (prepared == nullptr) {
      ADD_FAILURE() << "Boolean prohibited preparation returned null";
      return std::unique_ptr<DocSet>();
    }
    return QueryPrep::materialize(*prepared, segment, nullptr);
  };

  SkipStatsScope stats;
  auto built = run(false);
  ASSERT_NE(nullptr, built);
  EXPECT_EQ(1, built->card());
  EXPECT_TRUE(built->get(1));
  EXPECT_EQ(1, SkipStats::prohibitedCachePullBuilds);
  SkipStats::reset();
  // A mandatory wrapper that only needs prepare must retain lazy exclusion.
  // Folding would bypass the prohibited supplier and its existing counter.
  auto hit = run(true);
  ASSERT_NE(nullptr, hit);
  EXPECT_EQ(1, hit->card());
  EXPECT_TRUE(hit->get(1));
  EXPECT_EQ(1, SkipStats::prohibitedCachePullHits);
}

TEST(FilterCacheTest, recursiveRawPublicationServesPinnedOldReader) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addIdTermDoc(writer, "0", "body selected");
  addIdTermDoc(writer, "1", "body selected");
  addIdTermDoc(writer, "2", "body");
  writer.commit();
  auto oldReader = writer.snapshots.readers.getReader(0);
  ASSERT_EQ(1u, oldReader->segments().size());
  ASSERT_EQ(nullptr, oldReader->segments()[0].liveDocs());

  auto& deletes = writer.obtainInverter();
  deletes.deleteId("0", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto newReader = writer.snapshots.readers.getReader(0);
  ASSERT_NE(oldReader, newReader);
  ASSERT_EQ(oldReader->segments()[0].segInfo.seg_id,
            newReader->segments()[0].segInfo.seg_id);
  ASSERT_NE(nullptr, newReader->segments()[0].liveDocs());

  TermQuery selected("text_w", "selected");
  TermQuery body("text_w", "body");
  std::array<Query*, 1> mandatory{&body};
  std::array<Query*, 1> filters{&selected};
  BooleanQuery outer(mandatory, {}, {}, filters);

  {
    MemPool contextPool;
    Query::Context context(contextPool, *newReader);
    auto* weight = selected.createWeight(context, 0);
    auto* use = context.getFilterUse(selected);
    MemPool execPool;
    ASSERT_NE(nullptr, QueryPrep::filterSupplier(
        execPool, *weight, use, *newReader, newReader->segments()[0],
        QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE));
  }

  {
    MemPool contextPool;
    Query::Context context(contextPool, *newReader);
    auto* weight = outer.createWeight(context, 0);
    auto* use = context.getFilterUse(outer);
    auto effective = QueryPrep::materializeEffectiveFilter(
        *weight, use, *newReader, newReader->segments()[0], nullptr);
    ASSERT_NE(nullptr, use->rawDocSet(0));
    EXPECT_EQ(2, use->rawDocSet(0)->card());
    ASSERT_NE(nullptr, effective.get());
    EXPECT_EQ(1, effective.get()->card());
  }

  auto beforeOld = writer.snapshots.readers.filterCache->counters();
  {
    MemPool contextPool;
    Query::Context context(contextPool, *oldReader);
    auto* weight = outer.createWeight(context, 0);
    auto* use = context.getFilterUse(outer);
    auto effective = QueryPrep::materializeEffectiveFilter(
        *weight, use, *oldReader, oldReader->segments()[0], nullptr);
    ASSERT_NE(nullptr, effective.get());
    EXPECT_EQ(2, effective.get()->card());
    EXPECT_TRUE(effective.get()->get(0));
    EXPECT_GT(writer.snapshots.readers.filterCache->counters().hits, beforeOld.hits);
  }
}

TEST(FilterCacheTest, recursiveRawBuildExceptionReleasesBothClaims) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir), config);
  IndexWriter writer(writerSnapshots, {}, nullptr);
  addTermDoc(writer, "body selected");
  addTermDoc(writer, "body");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader(0);

  TermQuery selected("text_w", "selected");
  FailSecondSupplierQuery failing(selected);
  TermQuery body("text_w", "body");
  std::array<Query*, 1> mandatory{&body};
  std::array<Query*, 1> filters{&failing};
  BooleanQuery outer(mandatory, {}, {}, filters);

  MemPool contextPool;
  Query::Context context(contextPool, *reader);
  auto* weight = outer.createWeight(context, 0);
  auto* outerUse = context.getFilterUse(outer);
  auto* innerUse = context.getFilterUse(failing);
  EXPECT_THROW(QueryPrep::materializeEffectiveFilter(
      *weight, outerUse, *reader, reader->segments()[0], nullptr),
      std::runtime_error);
  EXPECT_EQ(2, failing.calls());
  EXPECT_EQ(0u, writer.snapshots.readers.filterCache->counters().builds);

  auto outerRetry = outerUse->probe(0);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, outerRetry.kind());
  auto innerRetry = innerUse->probe(0);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, innerRetry.kind());
}

TEST(FilterCacheTest, readerStableUseRejectsRawByproductPublication) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 32}};
  cache.onReaderPublished(1, 11, segments);
  FilterCache::UseRegistry request(cache, 1, 11, segments);
  auto* use = request.get(FilterKey("reader-live-exact"),
                          FilterKeyScope::READER_STABLE);

  EXPECT_THROW(use->offerRaw(0, docs(32, {1, 3}), 1), std::logic_error);
  EXPECT_EQ(1u, cache.counters().publishRejects);
  EXPECT_EQ(0u, cache.bytesUsed());
}

TEST(FilterCacheTest, readerProbeCountsDeferredBypassesAndMisses) {
  LuxirConfig nodeConfig;
  nodeConfig.queryCacheBytes = 0;
  LuxirNode node(nodeConfig);
  luxir::test::CollectionHelper helper(node, "filter_cache_reader_counters");
  FilterCacheConfig config = testConfig();
  config.maxEntryBytes = 16;
  config.admissionThreshold = 2;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);

  FilterCache::UseRegistry missedRequest(*cache, *reader);
  auto* missed = missedRequest.get(
      FilterKey("small"), FilterKeyScope::READER_STABLE);
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
            missed->probeReaderStable(*reader, domains).kind());
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
            missed->probeReaderStable(*reader, domains).kind());
  EXPECT_EQ(1u, cache->counters().misses);
  EXPECT_EQ(0u, cache->counters().admissions);

  FilterCache::UseRegistry oversizedRequest(*cache, *reader);
  auto* oversized = oversizedRequest.get(
      FilterKey(std::string(32, 'x')), FilterKeyScope::READER_STABLE);
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
            oversized->probeReaderStable(*reader, domains).kind());
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
            oversized->probeReaderStable(*reader, domains).kind());
  EXPECT_EQ(2u, cache->counters().oversizedKeyBypasses);
  EXPECT_EQ(0u, cache->entryCountForTest());
}

TEST(FilterCacheTest, readerValueParticipatesInRecentWorkEviction) {
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
  IndexWriter writer(writerSnapshots);
  writer.mergePolicy->setMergeFactor(100);
  addTermDoc(writer, "first");
  addTermDoc(writer, "second");
  writer.commit();
  addTermDoc(writer, "third");
  addTermDoc(writer, "fourth");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  ASSERT_EQ(2u, reader->segments().size());
  auto domains = canonicalDomains(*reader);

  auto publishReader = [&](FilterCache& cache, const FilterKey& key,
                           uint64_t buildCostNanos) {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    auto probe = use->probeReaderStable(*reader, domains, FilterCache::Work::nanoseconds(buildCostNanos));
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
    return use->publishReaderStable(
        probe, oneDocPerSegment(*reader), 1000);
  };
  auto publishSegment = [&](FilterCache& cache, const FilterKey& key,
                            uint64_t buildCostNanos) {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key);
    auto probe = use->probe(0, FilterCache::Work::nanoseconds(buildCostNanos));
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    return use->publishRaw(
        0, probe, bitDocs(reader->segments()[0].maxDoc(), 0),
        1000);
  };

  FilterCacheConfig sizingConfig = testConfig();
  sizingConfig.admissionThreshold = 1;
  FilterCache sizing(sizingConfig);
  sizing.onReaderPublished(*reader);
  auto sizingReader = publishReader(
      sizing, FilterKey("sizing-reader"), 1);
  auto sizingSegment = publishSegment(
      sizing, FilterKey("sizing-segment"), 1);
  size_t readerCharge = sizingReader->ramBytesUsed();
  size_t segmentCharge = sizingSegment->ramBytesUsed();

  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 1;
  config.maxEntryBytes = std::max(readerCharge, segmentCharge) + 1;
  config.maxBytes = readerCharge + segmentCharge - 1;

  FilterCache cache(config);
  cache.onReaderPublished(*reader);
  FilterKey readerKey("reader-cheap");
  FilterKey segmentKey("segment-expensive");
  auto readerValue = publishReader(cache, readerKey, 1);
  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(readerKey, FilterKeyScope::READER_STABLE);
    use->probeReaderStable(*reader, domains, FilterCache::Work::nanoseconds(10000));
    use->probeReaderStable(*reader, domains, FilterCache::Work::nanoseconds(10000));
  }
  auto segmentValue = publishSegment(cache, segmentKey, 1);

  EXPECT_EQ(readerCharge, cache.bytesUsed());
  EXPECT_EQ(1u, cache.counters().evictions);
  EXPECT_EQ(0u, cache.counters().readerStableRetires);
  EXPECT_NE(nullptr, readerValue);
  EXPECT_NE(nullptr, segmentValue);
  auto value = ledgerFor(cache, readerKey);
  EXPECT_EQ(10001, value.recent);
  EXPECT_EQ(10000, value.creditedTotal);
  FilterCache::UseRegistry verify(cache, *reader);
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::HIT,
            verify.get(readerKey, FilterKeyScope::READER_STABLE)
                ->probeReaderStable(*reader, domains).kind());
  EXPECT_NE(FilterCache::Probe::Kind::HIT,
            verify.get(segmentKey)->probe(0).kind());
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, readerEvictionDebtSurvivesRefreshAndIsRepaid) {
  LuxirConfig nodeConfig;
  nodeConfig.queryCacheBytes = 0;
  LuxirNode node(nodeConfig);
  luxir::test::CollectionHelper helper(node, "filter_cache_reader_backoff");
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto writer = helper.getIndexWriter();
  auto reader = writer->snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);

  FilterCacheConfig sizingConfig = testConfig();
  sizingConfig.admissionThreshold = 1;
  FilterCache sizing(sizingConfig);
  sizing.onReaderPublished(*reader);
  FilterCache::UseRegistry sizingRequest(sizing, *reader);
  auto* sizingUse = sizingRequest.get(
      FilterKey("reader-sizing"), FilterKeyScope::READER_STABLE);
  auto sizingProbe = sizingUse->probeReaderStable(*reader, domains);
  ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, sizingProbe.kind());
  size_t charge = sizingUse->publishReaderStable(
      sizingProbe, oneDocPerSegment(*reader), 1)->ramBytesUsed();

  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = 2 * charge;
  config.maxBytes = 2 * charge - 1;

  auto cache = std::make_shared<FilterCache>(config);
  writer->snapshots.readers.filterCache = cache;
  cache->onReaderPublished(*reader);

  auto publish = [&](IndexReader& targetReader, const FilterKey& key,
                     uint32_t cost) {
    auto targetDomains = canonicalDomains(targetReader);
    FilterCache::UseRegistry request(*cache, targetReader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    auto probe = use->probeReaderStable(targetReader, targetDomains);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
    return use->publishReaderStable(
        probe, oneDocPerSegment(targetReader), cost);
  };

  FilterKey victim("reader-victim");
  FilterKey keeper("reader-keeper");
  publish(*reader, victim, 1);
  publish(*reader, keeper, std::numeric_limits<uint32_t>::max());
  ASSERT_EQ(1u, cache->counters().unrepaidEvictions);
  EXPECT_EQ(1u, cache->counters().unpaidBuildNanos);
  EXPECT_EQ(1ULL + std::numeric_limits<uint32_t>::max(),
            cache->counters().buildNanos);

  for (int bypass = 0; bypass < 1; bypass++) {
    FilterCache::UseRegistry request(*cache, *reader);
    auto probe = request.get(victim, FilterKeyScope::READER_STABLE)
        ->probeReaderStable(*reader, domains);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS, probe.kind());
  }
  ASSERT_EQ(1u, cache->counters().populationDeferred);

  std::vector<std::string> deletes{"1"};
  helper.deleteByIds(deletes, UpdateMessage::COMMIT);
  auto nextReader = writer->snapshots.readers.getReader();
  ASSERT_GT(nextReader->commitTime(), reader->commitTime());
  auto nextDomains = canonicalDomains(*nextReader);
  EXPECT_EQ(0u, cache->bytesUsed());

  publish(*nextReader, victim, 1);
  {
    FilterCache::UseRegistry hitRequest(*cache, *nextReader);
    auto hit = hitRequest.get(victim, FilterKeyScope::READER_STABLE)
        ->probeReaderStable(*nextReader, nextDomains);
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::HIT, hit.kind());
  }

  {
    FilterCache::UseRegistry secondHit(*cache, *nextReader);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::HIT,
        secondHit.get(victim, FilterKeyScope::READER_STABLE)
            ->probeReaderStable(*nextReader, nextDomains).kind());
  }
  EXPECT_EQ(2u, cache->counters().estimatedSavedNanos);
  size_t resident = 0;
  auto rows = cache->dump(10, &resident);
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ(1u, rows[0].buildNanos);

  evictValues(*cache);
  EXPECT_EQ(1u, cache->counters().unrepaidEvictions)
      << "a shared HIT must prevent new zero-hit debt";
  FilterCache::UseRegistry retryRequest(*cache, *nextReader);
  auto retry = retryRequest.get(victim, FilterKeyScope::READER_STABLE)
      ->probeReaderStable(*nextReader, nextDomains);
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD, retry.kind());
  EXPECT_EQ(1u, cache->counters().populationDeferred);
  ASSERT_NO_THROW(cache->validateForTest());
}

TEST(FilterCacheTest, wholeReaderPlanRejectsNestedDomainWithoutSighting) {
  LuxirConfig nodeConfig;
  nodeConfig.queryCacheBytes = 0;
  LuxirNode node(nodeConfig);
  luxir::test::CollectionHelper helper(node, "filter_cache_whole_knn_gate");
  installVectorSchema(helper.collection());
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 8; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  auto* fieldType = dynamic_cast<VectorFieldType*>(
      schema->getFieldTypePtr("embedding_v"));
  ASSERT_NE(nullptr, fieldType);
  std::array<float, 2> queryVector{0.0f, 0.0f};
  KnnQuery query("embedding_v", *fieldType, queryVector, 3, 0, 0, 0.0f,
                 /*exact=*/true);

  int64_t prepares = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  for (int repeat = 0; repeat < 2; repeat++) {
    MemPool pool;
    FilterKeyContext keyContext{.schemaGen = schema->gen_,
                                .timeZone = {}};
    Query::Context context(pool, *reader, {}, nullptr, keyContext);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(
        query, FilterKeyScope::READER_STABLE,
        FilterCache::AdmissionLane::WHOLE);
    ASSERT_NE(nullptr, use);
    QueryPrep::WholeMembershipPlan plan(
        *weight, use, context.filterUses,
        PreparedDomainDependence::QUERY_CANONICAL,
        QueryPrep::WholeMembershipConsumer::COUNT);
    std::vector<std::unique_ptr<DocSet>> ownedDomains;
    std::vector<DocSet*> nestedDomains;
    for (auto& segment : reader->segments()) {
      ownedDomains.push_back(docs(segment.maxDoc(), {0}));
      nestedDomains.push_back(ownedDomains.back().get());
    }
    Query::Weight::PrepareContext prepareContext{
        *reader, nestedDomains, nullptr, nullptr};
    auto prepared = plan.prepareMainWeight(prepareContext);
    ASSERT_NE(nullptr, prepared);
    auto result = test::resolveWholeMembershipForTests(plan,
        *reader, reader->segments()[0], nestedDomains[0], prepared.get());
    EXPECT_FALSE(result.available);
  }
  {
    MemPool pool;
    FilterKeyContext keyContext{.schemaGen = schema->gen_, .timeZone = {}};
    Query::Context context(pool, *reader, {}, nullptr, keyContext);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(
        query, FilterKeyScope::READER_STABLE,
        FilterCache::AdmissionLane::WHOLE);
    QueryPrep::WholeMembershipPlan plan(
        *weight, use, context.filterUses,
        PreparedDomainDependence::PREPARE_DOMAIN,
        QueryPrep::WholeMembershipConsumer::COUNT);
    auto rootDomains = canonicalDomains(*reader);
    Query::Weight::PrepareContext prepareContext{
        *reader, rootDomains, nullptr, nullptr};
    EXPECT_THROW(plan.prepareMainWeight(prepareContext), std::logic_error);
  }
  EXPECT_EQ(prepares + 2,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  EXPECT_EQ(0u, cache->counters().admissions);
  EXPECT_EQ(0u, cache->counters().buildAttempts);
  EXPECT_EQ(0u, cache->entryCountForTest());
}

TEST(FilterCacheTest, readerPublicationRetiresAndRejectsLateKnnValue) {
  LuxirConfig nodeConfig;
  nodeConfig.queryCacheBytes = 0;
  LuxirNode node(nodeConfig);
  luxir::test::CollectionHelper helper(node, "filter_cache_reader_retire");
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  auto identities = readerIdentitiesForTest(*reader);

  FilterKey residentKey("reader-resident");
  FilterCache::UseRegistry residentRequest(*cache, *reader);
  auto* residentUse = residentRequest.get(
      residentKey, FilterKeyScope::READER_STABLE);
  auto residentClaim = residentUse->probeReaderStable(*reader, domains);
  ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, residentClaim.kind());
  EXPECT_EQ(0u, cache->counters().builds);
  auto residentValue = residentUse->publishReaderStable(
      residentClaim, oneDocPerSegment(*reader),
      std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(1u, cache->counters().builds);
  EXPECT_THROW(residentValue->docSet(
      0, {reader->segments()[0].segInfo.seg_id + 1,
          reader->segments()[0].maxDoc()}), std::logic_error);
  ASSERT_GT(cache->bytesUsed(), 0u);
  const void* entryIdentity = cache->entryIdentityForTest(residentKey);

  FilterKey lateKey("reader-late");
  FilterCache::UseRegistry lateRequest(*cache, *reader);
  auto* lateUse = lateRequest.get(lateKey, FilterKeyScope::READER_STABLE);
  auto lateClaim = lateUse->probeReaderStable(*reader, domains);
  ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, lateClaim.kind());

  uint64_t nextVersion = reader->commitTime() + 1;
  ASSERT_TRUE(cache->onReaderPublished(
      reader->coreGen(), nextVersion, identities));
  EXPECT_EQ(0u, cache->bytesUsed());
  EXPECT_EQ(1u, cache->counters().readerStableRetires);
  EXPECT_EQ(entryIdentity, cache->entryIdentityForTest(residentKey));

  lateUse->publishReaderStable(
      lateClaim, oneDocPerSegment(*reader), 1);
  EXPECT_EQ(0u, cache->bytesUsed());
  EXPECT_EQ(1u, cache->counters().publishRejects);
  ASSERT_NO_THROW(cache->validateForTest());
}

TEST(FilterCacheTest, routedAccountingOwnsRejectedReaderValue) {
  RAMDir dir;
  CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
  IndexWriter writer(writerSnapshots);
  addTermDoc(writer, "body");
  writer.commit();
  auto reader = writer.snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  auto identities = readerIdentitiesForTest(*reader);

  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  cache.onReaderPublished(*reader);
  RequestMemTracker tracker(0);

  {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(
        FilterKey("routed-stale-reader"), FilterKeyScope::READER_STABLE);
    use->enableRoutedAccounting(0, tracker, "segment 0");
    auto claim = use->probeReaderStable(*reader, domains);
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, claim.kind());

    ASSERT_TRUE(cache.onReaderPublished(
        reader->coreGen(), reader->commitTime() + 1, identities));
    auto value = use->publishReaderStable(
        claim, oneDocPerSegment(*reader), 1);
    EXPECT_EQ(value->ramBytesUsed(), tracker.bytes());
    EXPECT_EQ(0u, cache.bytesUsed());
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST(FilterCacheTest, readerPublishRaceCannotResurrectStaleValue) {
  constexpr int ENTRY_COUNT = 128;
  LuxirConfig nodeConfig;
  nodeConfig.queryCacheBytes = 0;
  LuxirNode node(nodeConfig);
  luxir::test::CollectionHelper helper(node, "filter_cache_reader_race");
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxMetadataEntries = ENTRY_COUNT * 2;
  config.maxMetadataBytes = 1024 * 1024;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto domains = canonicalDomains(*reader);
  auto identities = readerIdentitiesForTest(*reader);

  std::vector<std::unique_ptr<FilterCache::UseRegistry>> requests;
  std::vector<FilterCache::Use*> uses;
  std::vector<FilterCache::ReaderProbe> claims;
  requests.reserve(ENTRY_COUNT);
  uses.reserve(ENTRY_COUNT);
  claims.reserve(ENTRY_COUNT);
  for (int i = 0; i < ENTRY_COUNT; i++) {
    requests.push_back(std::make_unique<FilterCache::UseRegistry>(
        *cache, *reader));
    auto* use = requests.back()->get(
        FilterKey("reader-race-" + std::to_string(i)),
        FilterKeyScope::READER_STABLE);
    uses.push_back(use);
    claims.push_back(use->probeReaderStable(*reader, domains));
    ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD,
              claims.back().kind());
  }

  std::barrier start(2);
  std::thread publisher([&] {
    start.arrive_and_wait();
    for (int i = 0; i < ENTRY_COUNT; i++) {
      uses[(size_t)i]->publishReaderStable(
          claims[(size_t)i], oneDocPerSegment(*reader), 1);
    }
  });
  std::thread publication([&] {
    start.arrive_and_wait();
    cache->onReaderPublished(reader->coreGen(), reader->commitTime() + 1,
                             identities);
  });
  publisher.join();
  publication.join();

  auto counters = cache->counters();
  EXPECT_EQ(0u, counters.residentBytes);
  EXPECT_EQ(ENTRY_COUNT,
            counters.readerStableRetires + counters.publishRejects);
  EXPECT_EQ((size_t)ENTRY_COUNT, cache->entryCountForTest());
  ASSERT_NO_THROW(cache->validateForTest());
}

TEST(DocSetScorerTest, windowFilterSupportsBitsetAndArray) {
  RAMBitDocSet bitset(192);
  bitset.mutableBits().set(3);
  bitset.mutableBits().set(65);
  bitset.mutableBits().set(129);
  std::vector<int32_t> values{3, 65, 129};
  ArrDocSet array(std::move(values));

  auto check = [](DocSet& docs) {
    MemPool pool;
    QueryPrep::DocSetScorer scorer(&docs, 192);
    std::array<Query::Scorer*, 1> scorers{&scorer};
    WindowFilter filter(pool, scorers);
    EXPECT_EQ(1, filter.prepare(1, 64));
    EXPECT_TRUE(filter.accepts(3));
    EXPECT_EQ(2, filter.prepare(64, 130));
    EXPECT_TRUE(filter.accepts(65));
    EXPECT_TRUE(filter.accepts(129));
  };
  check(bitset);
  check(array);
}

TEST(DocSetScorerTest, bitsetAndArrayAdvanceVisitOnlyMembers) {
  constexpr int32_t maxDoc = 1 << 20;
  RAMBitDocSet bitset(maxDoc);
  bitset.mutableBits().set(3);
  bitset.mutableBits().set(65537);
  bitset.mutableBits().set(maxDoc - 1);
  ArrDocSet array({3, 65537, maxDoc - 1});

  auto check = [](DocSet& docs) {
    QueryPrep::DocSetScorer scorer(&docs, maxDoc);
    EXPECT_EQ(3, scorer.next());
    EXPECT_EQ(65537, scorer.advance(4096));
    EXPECT_EQ(maxDoc - 1, scorer.next());
    EXPECT_EQ(PostingsReader::END, scorer.next());
  };
  check(bitset);
  check(array);
}

TEST(DocSetScorerTest, bulkScorerCountsAndEmitsBitsetAndArray) {
  RAMBitDocSet bitset(192);
  bitset.mutableBits().set(3);
  bitset.mutableBits().set(65);
  bitset.mutableBits().set(129);
  ArrDocSet array({3, 65, 129});
  RAMBitDocSet filter(192);
  filter.mutableBits().set(65);
  filter.mutableBits().set(129);

  auto check = [&](DocSet& source) {
    MemPool countPool;
    DocSetBulkScorer counter(countPool, &source, 192);
    DocSetBuilder builder(192);
    int64_t count = 0;
    EXPECT_EQ(PostingsReader::END, counter.countNextWindow(
        count, &builder, &filter, 0, 192));
    auto counted = builder.build();
    EXPECT_EQ(2, count);
    EXPECT_TRUE(counted->get(65));
    EXPECT_TRUE(counted->get(129));

    MemPool scorePool;
    DocSetBulkScorer scorer(scorePool, &source, 192);
    ScoreWindow window;
    EXPECT_EQ(PostingsReader::END, scorer.scoreNextWindow(
        window, &filter, 0, 192, std::numeric_limits<float>::lowest()));
    EXPECT_EQ(2, window.size);
    EXPECT_EQ(65, window.docs[0]);
    EXPECT_EQ(129, window.docs[1]);

    MemPool exactPool;
    DocSetBulkScorer exact(exactPool, &source, 192);
    int64_t exactCount = 0;
    EXPECT_EQ(PostingsReader::END, exact.countNextWindow(
        exactCount, nullptr, nullptr, 0, 192));
    EXPECT_EQ(3, exactCount);
  };

  check(bitset);
  check(array);
}

TEST(DocSetScorerTest, bulkScorerPreservesSparseWindowsAndDomains) {
  constexpr int32_t windowSize = DocsEnumMeta::L1_DOCS;
  constexpr int32_t maxDoc = 4 * windowSize + 37;
  std::vector<int32_t> sourceDocs{
      1, windowSize - 1, windowSize, windowSize + 7,
      2 * windowSize - 1, 3 * windowSize + 3, maxDoc - 1};
  ArrDocSet arraySource{std::vector<int32_t>(sourceDocs)};
  RAMBitDocSet bitSource(maxDoc);
  for (int32_t doc : sourceDocs) {
    bitSource.mutableBits().set(doc);
  }

  RAMBitDocSet bitFilter(maxDoc);
  for (int32_t doc : {windowSize - 1, windowSize,
                      2 * windowSize - 1, maxDoc - 1}) {
    bitFilter.mutableBits().set(doc);
  }
  ArrDocSet arrayFilter(
      {1, windowSize + 7, 3 * windowSize + 3});

  struct CountResult {
    int64_t count;
    std::unique_ptr<DocSet> domain;
    bool sawEmptyWindow;
  };
  auto countWindows = [&](DocSet& source, DocSet* filter) {
    MemPool pool;
    DocSetBulkScorer scorer(pool, &source, maxDoc);
    DocSetBuilder builder(maxDoc);
    int64_t count = 0;
    bool sawEmptyWindow = false;
    for (int32_t cursor = 0; cursor != PostingsReader::END; ) {
      int64_t before = count;
      int32_t next = scorer.countNextWindow(
          count, &builder, filter, cursor, maxDoc);
      sawEmptyWindow |= count == before;
      if (next == PostingsReader::END) {
        break;
      }
      EXPECT_GT(next, cursor);
      cursor = next;
    }
    return CountResult{count, builder.build(), sawEmptyWindow};
  };
  auto expectedFor = [&](DocSet* filter) {
    std::vector<int32_t> expected;
    for (int32_t doc : sourceDocs) {
      if (filter == nullptr || filter->get(doc)) {
        expected.push_back(doc);
      }
    }
    return expected;
  };
  auto expectCount = [&](DocSet& source, DocSet* filter,
                         bool expectEmptyWindow) {
    CountResult result = countWindows(source, filter);
    std::vector<int32_t> expected = expectedFor(filter);
    std::vector<int32_t> actual;
    for (int32_t doc = 0; doc < maxDoc; doc++) {
      if (result.domain->get(doc)) {
        actual.push_back(doc);
      }
    }
    EXPECT_EQ((int64_t) expected.size(), result.count);
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(expectEmptyWindow, result.sawEmptyWindow);
  };

  expectCount(arraySource, nullptr, true);
  expectCount(arraySource, &bitFilter, true);
  expectCount(arraySource, &arrayFilter, true);
  expectCount(bitSource, nullptr, true);
  expectCount(bitSource, &arrayFilter, true);

  {
    MemPool pool;
    DocSetBulkScorer scorer(pool, &arraySource, maxDoc);
    DocSetBuilder builder(maxDoc);
    int64_t count = 0;
    constexpr int32_t rangeMin = windowSize - 2;
    constexpr int32_t rangeMax = 2 * windowSize + 1;
    for (int32_t cursor = rangeMin; cursor != PostingsReader::END; ) {
      int32_t next = scorer.countNextWindow(
          count, &builder, nullptr, cursor, rangeMax);
      if (next == PostingsReader::END) {
        break;
      }
      ASSERT_GT(next, cursor);
      cursor = next;
    }
    auto domain = builder.build();
    EXPECT_EQ(4, count);
    ASSERT_EQ(DocSet::ARRAY, domain->type);
    std::span<const int32_t> domainDocs =
        ((ArrDocSet*) domain.get())->docs();
    EXPECT_EQ(
        (std::vector<int32_t>{
            windowSize - 1, windowSize, windowSize + 7,
            2 * windowSize - 1}),
        std::vector<int32_t>(domainDocs.begin(), domainDocs.end()));
  }

  auto expectScores = [&](DocSet* filter) {
    MemPool pool;
    DocSetBulkScorer scorer(pool, &arraySource, maxDoc);
    std::vector<int32_t> actual;
    for (int32_t cursor = 0; cursor != PostingsReader::END; ) {
      ScoreWindow window;
      int32_t next = scorer.scoreNextWindow(
          window, filter, cursor, maxDoc, 0.0f);
      actual.insert(actual.end(), window.docs.begin(), window.docs.end());
      for (float score : window.scores) {
        EXPECT_EQ(0.0f, score);
      }
      if (next == PostingsReader::END) {
        break;
      }
      EXPECT_GT(next, cursor);
      cursor = next;
    }
    EXPECT_EQ(expectedFor(filter), actual);
  };
  expectScores(&bitFilter);
  expectScores(&arrayFilter);
}

// The null-source form: all docs in [0, maxDoc), with the per-call filter
// consumed as the window source (match-all intersect filter = filter).
TEST(DocSetScorerTest, nullSourceBulkScorerEmitsAllDocs) {
  constexpr int32_t windowSize = DocsEnumMeta::L1_DOCS;
  constexpr int32_t maxDoc = 2 * windowSize + 41;

  {
    MemPool pool;
    DocSetBulkScorer scorer(pool, nullptr, maxDoc, 2.5f);
    EXPECT_TRUE(scorer.supportsMatchWindows());
    ScoreWindow window;
    int32_t next = scorer.matchNextWindow(window, nullptr, 0, maxDoc);
    EXPECT_EQ(windowSize, next);
    ASSERT_EQ(windowSize, window.size);
    EXPECT_EQ(0, window.docs[0]);
    EXPECT_EQ(windowSize - 1, window.docs[(size_t) windowSize - 1]);
    next = scorer.matchNextWindow(window, nullptr, 2 * windowSize, maxDoc);
    EXPECT_EQ(PostingsReader::END, next);
    ASSERT_EQ(41, window.size);
    EXPECT_EQ(2 * windowSize, window.docs[0]);
    EXPECT_EQ(maxDoc - 1, window.docs[40]);
  }

  // Scored windows carry the constant score; a higher competitive floor
  // produces empty windows but still advances.
  {
    MemPool pool;
    DocSetBulkScorer scorer(pool, nullptr, maxDoc, 2.5f);
    ScoreWindow window;
    scorer.scoreNextWindow(window, nullptr, 5, maxDoc, 2.5f);
    ASSERT_EQ(windowSize, window.size);
    EXPECT_EQ(5, window.docs[0]);
    EXPECT_EQ(2.5f, window.scores[0]);
    int32_t next = scorer.scoreNextWindow(
        window, nullptr, windowSize + 5, maxDoc, 3.0f);
    EXPECT_EQ(0, window.size);
    EXPECT_EQ(2 * windowSize + 5, next);
  }

  // Bitset and array filters become the source.
  {
    RAMBitDocSet bits(maxDoc);
    bits.mutableBits().set(3);
    bits.mutableBits().set(windowSize);
    bits.mutableBits().set(maxDoc - 1);
    ArrDocSet arr({3, windowSize, maxDoc - 1});
    for (DocSet* filter : {(DocSet*) &bits, (DocSet*) &arr}) {
      MemPool pool;
      DocSetBulkScorer scorer(pool, nullptr, maxDoc);
      std::vector<int32_t> got;
      for (int32_t cursor = 0; cursor != PostingsReader::END; ) {
        ScoreWindow window;
        int32_t next = scorer.matchNextWindow(window, filter, cursor, maxDoc);
        got.insert(got.end(), window.docs.begin(), window.docs.end());
        if (next == PostingsReader::END) break;
        ASSERT_GT(next, cursor);
        cursor = next;
      }
      EXPECT_EQ((std::vector<int32_t>{3, windowSize, maxDoc - 1}), got);
    }
  }

  // Counting: arithmetic remaining-range shortcut, filter-card shortcut, and
  // mid-range domain building.
  {
    MemPool pool;
    DocSetBulkScorer scorer(pool, nullptr, maxDoc);
    int64_t count = 0;
    EXPECT_EQ(PostingsReader::END,
              scorer.countNextWindow(count, nullptr, nullptr, 7, maxDoc));
    EXPECT_EQ(maxDoc - 7, count);

    ArrDocSet arr({3, windowSize, maxDoc - 1});
    count = 0;
    EXPECT_EQ(PostingsReader::END,
              scorer.countNextWindow(count, nullptr, &arr, 0, maxDoc));
    EXPECT_EQ(3, count);

    DocSetBuilder builder(maxDoc);
    count = 0;
    for (int32_t cursor = windowSize - 2; cursor != PostingsReader::END; ) {
      int32_t next = scorer.countNextWindow(
          count, &builder, nullptr, cursor, windowSize + 2);
      if (next == PostingsReader::END) break;
      ASSERT_GT(next, cursor);
      cursor = next;
    }
    EXPECT_EQ(4, count);
    auto domain = builder.build();
    EXPECT_TRUE(domain->get(windowSize - 2));
    EXPECT_TRUE(domain->get(windowSize + 1));
    EXPECT_FALSE(domain->get(windowSize + 2));
  }
}

TEST(FilterCacheIntegrationTest, cacheControlIncludesLedgerGhosts) {
  LuxirNode node;
  luxir::test::CollectionHelper helper(node, "cache_ledger_dump");
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1"), luxir::test::flatdoc("id", "2")},
      UpdateMessage::COMMIT).success);
  auto config = testConfig();
  config.admissionThreshold = 1;
  config.recentWorkEpochs = 1;
  auto cache = std::make_shared<FilterCache>(config);
  auto writer = helper.getIndexWriter();
  writer->snapshots.readers.filterCache = cache;
  auto reader = writer->snapshots.readers.getReader();
  cache->onReaderPublished(*reader);
  FilterCache::UseRegistry populate(*cache, *reader);
  auto* use = populate.get(FilterKey("dump-ledger"));
  auto build = use->probe(0, FilterCache::Work::nanoseconds(2000));
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, build.kind());
  use->publishRaw(0, build, docs(reader->segments()[0].maxDoc(), {0}), 4000);

  cache->sweep();
  for (int stage = 0; stage < 3; stage++) {
    if (stage == 1) {
      evictValues(*cache);
      FilterCache::UseRegistry request(*cache, *reader);
      auto* ghost = request.get(FilterKey("dump-ledger"));
      EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, ghost->probe(0, FilterCache::Work::pending()).kind());
      ghost->charge(0, FilterCache::Work::nanoseconds(1000));
    }
    std::pmr::monotonic_buffer_resource arena;
    api::CacheControlRequest request;
    request.collection = "cache_ledger_dump";
    request.dump = true;
    request.include_ghosts = true;
    request.flush = stage == 2;
    request.reset_counters = stage == 1;
    api::CacheControlResponse response;
    gatherCacheControl(node, request, response, arena);
    const auto& shard = response.collections[0].shards[0];
    if (stage == 2) {
      EXPECT_TRUE(shard.entries.empty());
      EXPECT_EQ(0u, shard.stats.outstanding_debt_us);
      continue;
    }
    ASSERT_EQ(1u, shard.entries.size());
    ASSERT_EQ(1u, shard.entries[0].values.size());
    const auto& value = shard.entries[0].values[0];
    EXPECT_EQ(stage == 0, value.resident);
    EXPECT_EQ(1u, value.raw_cardinality);
    EXPECT_DOUBLE_EQ(stage == 1 ? 1 : 0, value.owed_us);
    EXPECT_DOUBLE_EQ(4, value.debt_us);
    EXPECT_EQ(4u, value.production_us);
    EXPECT_EQ("measured", value.production_source);
    EXPECT_GT(value.reference_bytes, 0u);
    EXPECT_EQ(4u, shard.stats.outstanding_debt_us);
  }
}

TEST(FilterCacheIntegrationTest, cacheControlResizes) {
  LuxirConfig config;
  config.queryCacheBytes = 4 * 1024 * 1024;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_resize");
  ASSERT_TRUE(helper.indexAll({luxir::test::flatdoc("id", "1")},
                              UpdateMessage::COMMIT).success);
  auto cache = helper.getIndexWriter()->snapshots.readers.filterCache;

  auto resize = [&](uint64_t maxBytes) {
    std::pmr::monotonic_buffer_resource arena;
    api::CacheControlRequest request;
    request.collection = "filter_cache_resize";
    request.max_bytes = maxBytes;
    api::CacheControlResponse response;
    gatherCacheControl(node, request, response, arena);
    EXPECT_EQ(1u, response.collections.size());
    EXPECT_EQ(1u, response.collections[0].shards.size());
    const auto& stats = response.collections[0].shards[0].stats;
    EXPECT_EQ(maxBytes != 0, stats.enabled);
    EXPECT_EQ(maxBytes, stats.max_bytes);
  };
  resize(0);
  EXPECT_FALSE(cache->enabled());
  resize(1024 * 1024);
  EXPECT_EQ(1024u * 1024, cache->configuration().maxBytes);
}

TEST(FilterCacheIntegrationTest, cachedAndOffMatchAcrossDeleteAndFlush) {
  LuxirConfig onConfig;
  onConfig.queryCacheBytes = 4 * 1024 * 1024;
  LuxirConfig offConfig;
  offConfig.queryCacheBytes = 0;
  LuxirNode onNode(onConfig);
  LuxirNode offNode(offConfig);
  luxir::test::CollectionHelper on(onNode, "filter_cache_it");
  luxir::test::CollectionHelper off(offNode, "filter_cache_it");

  std::vector<luxir::test::Doc> docs;
  docs.reserve(1100);
  for (int32_t i = 0; i < 1100; i++) {
    docs.push_back(luxir::test::flatdoc(
        "id", std::to_string(i), "body_w", "body",
        "filter_w", (i & 1) == 0 ? "keep" : "drop",
        "group_s", (i % 3) == 0 ? "a" : "b"));
  }
  ASSERT_TRUE(on.indexAll(docs, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(off.indexAll(docs, UpdateMessage::COMMIT).success);
  auto onCache = on.getIndexWriter()->snapshots.readers.filterCache;
  auto offCache = off.getIndexWriter()->snapshots.readers.filterCache;
  ASSERT_TRUE(onCache->enabled());
  ASSERT_FALSE(offCache->enabled());

  auto registryRequest = luxir::test::localReq(offNode.getSearchEngine());
  registryRequest->collection("filter_cache_it")
      .topDocs("registry")
      .matchQuery("body_w", "body")
      .matchFilter("filter_w", "keep")
      .countLimit()
      .limit(0);
  registryRequest->execute();
  ASSERT_TRUE(registryRequest->ok()) << registryRequest->toString();
  EXPECT_NE(nullptr, registryRequest->filterUses);

  auto firstOn = runCachedSearch(onNode, "filter_cache_it", "keep");
  auto firstOff = runCachedSearch(offNode, "filter_cache_it", "keep");
  EXPECT_EQ(firstOff, firstOn);
  EXPECT_EQ(firstOff, runCachedSearch(onNode, "filter_cache_it", "keep"));
  EXPECT_EQ(firstOff, runCachedSearch(onNode, "filter_cache_it", "keep"));
  auto beforeMutation = onCache->counters();
  EXPECT_GT(beforeMutation.builds, 0u);
  EXPECT_GT(beforeMutation.hits, 0u);

  luxir::test::Doc replacement = luxir::test::flatdoc(
      "id", "replacement", "body_w", "body", "filter_w", "keep",
      "group_s", "a");
  luxir::test::CollectionHelper::UpdateBuilder onUpdate;
  onUpdate.remove("0").add(replacement).commit();
  ASSERT_TRUE(on.submit(onUpdate).success);
  luxir::test::CollectionHelper::UpdateBuilder offUpdate;
  offUpdate.remove("0").add(replacement).commit();
  ASSERT_TRUE(off.submit(offUpdate).success);

  auto afterOn = runCachedSearch(onNode, "filter_cache_it", "keep");
  auto afterOff = runCachedSearch(offNode, "filter_cache_it", "keep");
  EXPECT_EQ(afterOff, afterOn);
  EXPECT_EQ(firstOn.count, afterOn.count);
  EXPECT_NE(firstOn.ids, afterOn.ids);
  EXPECT_GT(onCache->counters().hits, beforeMutation.hits);
  auto offCounters = offCache->counters();
  EXPECT_EQ(0u, offCounters.hits);
  EXPECT_EQ(0u, offCounters.misses);
  EXPECT_EQ(0u, offCounters.admissions);
  EXPECT_EQ(0u, offCounters.builds);
}

TEST(FilterCacheIntegrationTest, dataResetReplacesRewoundCacheNamespace) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_reset";
  luxir::test::CollectionHelper helper(node, collection);
  auto writer = helper.getIndexWriter();
  FilterCacheConfig cacheConfig = testConfig();
  cacheConfig.minSegmentDocs = 1000;
  cacheConfig.admissionThreshold = 1;
  auto firstCache = std::make_shared<FilterCache>(cacheConfig);
  writer->snapshots.readers.filterCache = firstCache;

  std::vector<luxir::test::Doc> firstPhase;
  firstPhase.reserve(1000);
  for (int32_t i = 0; i < 1000; i++) {
    firstPhase.push_back(luxir::test::flatdoc(
        "id", "old-" + std::to_string(i), "body_w", "body",
        "filter_w", i < 50 ? "keep" : "drop"));
  }
  ASSERT_TRUE(helper.indexAll(firstPhase, UpdateMessage::COMMIT).success);
  EXPECT_EQ(50, runCachedSearch(node, collection, "keep").count);
  EXPECT_EQ(50, runCachedSearch(node, collection, "keep").count);
  ASSERT_GT(firstCache->counters().readerStableHits
                + firstCache->counters().hits,
            0u);

  writer->testDeleteAllData();
  // A namespace rewind rebuilds from the writer's CONSTRUCTION config, not
  // the installed cache's: a test-assigned policy must not outlive a reset.
  // This writer was constructed with the cache disabled (queryCacheBytes 0).
  auto autoCache = writer->snapshots.readers.filterCache;
  ASSERT_NE(firstCache, autoCache);
  EXPECT_FALSE(autoCache->enabled());
  // A test that wants cache-on after a reset installs its own again.
  auto secondCache = std::make_shared<FilterCache>(cacheConfig);
  writer->snapshots.readers.filterCache = secondCache;
  EXPECT_EQ(0u, secondCache->entryCountForTest());

  std::vector<luxir::test::Doc> secondPhase;
  std::vector<std::string> expectedIds;
  secondPhase.reserve(1000);
  for (int32_t i = 0; i < 1000; i++) {
    bool keep = i >= 950;
    std::string id = "new-" + std::to_string(i);
    secondPhase.push_back(luxir::test::flatdoc(
        "id", id, "body_w", "body",
        "filter_w", keep ? "keep" : "drop"));
    if (keep) expectedIds.push_back(id);
  }
  std::sort(expectedIds.begin(), expectedIds.end());
  ASSERT_TRUE(helper.indexAll(secondPhase, UpdateMessage::COMMIT).success);
  auto actual = runCachedSearch(node, collection, "keep");
  EXPECT_EQ(expectedIds, actual.ids);
  EXPECT_EQ(50, actual.count);
  auto counters = secondCache->counters();
  EXPECT_GT(counters.misses, 0u);
  EXPECT_EQ(0u, counters.hits);
  ASSERT_NO_THROW(secondCache->validateForTest());

  // The swap must propagate through READER succession, not just the writer
  // pointer: a post-swap reader carries the fresh cache, and a further
  // reader built WITH a previousReader must not resurrect the old one.
  auto postResetReader = writer->snapshots.readers.getReader();
  EXPECT_EQ(secondCache.get(), postResetReader->filterCache());
  luxir::test::CollectionHelper::UpdateBuilder touch;
  touch.remove("new-0").commit();
  ASSERT_TRUE(helper.submit(touch).success);
  auto successorReader = writer->snapshots.readers.getReader();
  ASSERT_NE(postResetReader.get(), successorReader.get());
  EXPECT_EQ(secondCache.get(), successorReader->filterCache());
}

TEST(FilterCacheIntegrationTest, membershipProjectionCachesScoreOnlyAndMembershipKnn) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_membership");
  auto writer = helper.getIndexWriter();
  auto cache = std::make_shared<FilterCache>(testConfig());
  writer->snapshots.readers.filterCache = cache;
  SchemaBuilder schema;
  auto& vector = schema.templ("_v");
  vector.type = api::FieldDef_::FieldClass::VECTOR;
  vector.column = true;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper.collection());

  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body", "filter_w", "keep",
                           "embedding_v", std::vector<float>{0.0f, 0.0f}),
      luxir::test::flatdoc("id", "2", "body_w", "body", "filter_w", "keep",
                           "embedding_v", std::vector<float>{10.0f, 0.0f}),
      luxir::test::flatdoc("id", "3", "body_w", "body", "filter_w", "drop",
                           "embedding_v", std::vector<float>{0.0f, 1.0f})},
      UpdateMessage::COMMIT).success);

  auto run = [&](bool requiredTerm) {
    auto request = luxir::test::localReq(node.getSearchEngine());
    auto& cursor = request->collection("filter_cache_membership")
                       .topDocs("q")
                       .allQuery()
                       .fields({"id"})
                       .countLimit()
                       .limit(-1);
    auto term = luxir::test::qb::match(cursor.mr(), "filter_w", "keep");
    auto knn = luxir::test::qb::knn(cursor.mr(), "embedding_v",
                                    {0.0f, 1.0f}, 1, 0, true);
    auto filter = requiredTerm
        ? luxir::test::qb::boolean(cursor.mr(), {term}, {knn})
        : luxir::test::qb::boolean(cursor.mr(), {}, {term, knn});
    addFilter(cursor, filter);
    request->execute();
    EXPECT_TRUE(request->ok()) << request->toString();
    CachedSearchResult result{
        .count = request->getMatchCount("q"), .ids = {}};
    for (const auto& doc : request->getDocs("q")) {
      auto* id = luxir::test::find(doc, "id");
      if (id != nullptr) result.ids.push_back(std::get<std::string>(*id));
    }
    std::sort(result.ids.begin(), result.ids.end());
    return result;
  };

  CachedSearchResult uncachedRequired = run(true);
  CachedSearchResult uncachedDisjunction = run(false);

  EXPECT_EQ(uncachedRequired, run(true));
  auto afterBuild = cache->counters();
  EXPECT_GT(afterBuild.builds, 0u);
  EXPECT_EQ(uncachedRequired, run(true));
  EXPECT_GT(cache->counters().hits, afterBuild.hits);

  auto beforeReaderBuild = cache->counters();
  EXPECT_EQ(uncachedDisjunction, run(false));
  auto afterReaderBuild = cache->counters();
  EXPECT_GT(afterReaderBuild.builds, beforeReaderBuild.builds);
  EXPECT_EQ(uncachedDisjunction, run(false));
  EXPECT_GT(cache->counters().readerStableHits,
            afterReaderBuild.readerStableHits);
}

TEST(FilterCacheIntegrationTest, knnRefreshKeepsEntryAndUsesCommitTime) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_knn_refresh");
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;

  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 24; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i), "group_s", i < 12 ? "a" : "b",
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  auto firstReader = helper.getIndexWriter()->snapshots.readers.getReader();
  std::array<float, 2> queryVector{0.0f, 0.0f};
  FilterKey key = knnKey(helper.collection(), *firstReader, queryVector, 4);

  CachedSearchResult uncached = runKnnFilter(
      node, "filter_cache_knn_refresh", queryVector, 4);
  EXPECT_EQ(uncached, runKnnFilter(
      node, "filter_cache_knn_refresh", queryVector, 4));
  EXPECT_EQ(uncached, runKnnFilter(
      node, "filter_cache_knn_refresh", queryVector, 4));
  auto warm = cache->counters();
  ASSERT_EQ(1u, warm.readerStableHits);
  const void* entryIdentity = cache->entryIdentityForTest(key);
  ASSERT_NE(nullptr, entryIdentity);

  luxir::test::CollectionHelper::UpdateBuilder update;
  update.remove("0").commit();
  ASSERT_TRUE(helper.submit(update).success);
  auto secondReader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_GT(secondReader->commitTime(), firstReader->commitTime());
  EXPECT_EQ(entryIdentity, cache->entryIdentityForTest(key));
  auto retired = cache->counters();
  EXPECT_EQ(warm.readerStableRetires + 1, retired.readerStableRetires);

  CachedSearchResult refreshed = runKnnFilter(
      node, "filter_cache_knn_refresh", queryVector, 4);
  EXPECT_EQ(entryIdentity, cache->entryIdentityForTest(key));
  auto rebuilt = cache->counters();
  EXPECT_EQ(retired.readerStableRefreshes + 1,
            rebuilt.readerStableRefreshes);
  EXPECT_EQ(refreshed, runKnnFilter(
      node, "filter_cache_knn_refresh", queryVector, 4));
  EXPECT_EQ(rebuilt.readerStableHits + 1,
            cache->counters().readerStableHits);
  EXPECT_EQ(4, refreshed.count);
  EXPECT_EQ(refreshed.ids.end(),
            std::find(refreshed.ids.begin(), refreshed.ids.end(), "0"));
  ASSERT_NO_THROW(cache->validateForTest());
}

TEST(FilterCacheIntegrationTest, schemaOnlyPublishKeepsWholeReaderCaching) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_schema_only");
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;

  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 24; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  auto firstReader = helper.getIndexWriter()->snapshots.readers.getReader();
  std::array<float, 2> queryVector{0.0f, 0.0f};
  CachedSearchResult expected = runKnnFilter(
      node, "filter_cache_schema_only", queryVector, 4);
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(expected, runKnnFilter(
        node, "filter_cache_schema_only", queryVector, 4));
  }
  auto warm = cache->counters();
  ASSERT_EQ(1u, warm.readerStableHits);

  // A schema-only publication keeps the segment files, so the reader reuses
  // the previous core and its cache version, under a new commit time.
  SchemaBuilder schema;
  auto& vector = schema.templ("_v");
  vector.type = api::FieldDef_::FieldClass::VECTOR;
  vector.column = true;
  vector.metric = api::VectorMetric::L2;
  schema.templ("_s").type = api::FieldDef_::FieldClass::STRING;
  schema.set(helper.collection());
  auto secondReader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_GT(secondReader->commitTime(), firstReader->commitTime());
  ASSERT_EQ(firstReader->segments().data(), secondReader->segments().data());
  EXPECT_EQ(firstReader->coreCommitTime(), secondReader->coreCommitTime());
  EXPECT_EQ(warm.readerStableRetires, cache->counters().readerStableRetires);

  // The schema change keys new entries, so admission, build and hit take
  // the same three requests as the warm-up did.
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(expected, runKnnFilter(
        node, "filter_cache_schema_only", queryVector, 4));
  }
  EXPECT_EQ(warm.readerStableHits + 1, cache->counters().readerStableHits);
  ASSERT_NO_THROW(cache->validateForTest());
}

TEST(FilterCacheIntegrationTest, fuzzyBoostsShareWholeCountFact) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_fuzzy_boost";
  luxir::test::CollectionHelper helper(node, collection);
  FilterCacheConfig cacheConfig = testConfig();
  cacheConfig.admissionThreshold = 1;
  cacheConfig.minSegmentDocs = 0;
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;

  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "color"),
      luxir::test::flatdoc("id", "2", "body_w", "colon"),
      luxir::test::flatdoc("id", "3", "body_w", "colors"),
      luxir::test::flatdoc("id", "4", "body_w", "other")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();

  auto count = [&](float boost) {
    MemPool pool;
    FilterKeyContext keyContext{.schemaGen = schema->gen_, .timeZone = {}};
    Query::Context context(pool, *reader, {}, nullptr, keyContext);
    FuzzyQuery query("body_w", "color", 1, 0, 20, boost);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(
        query, FilterCache::AdmissionLane::WHOLE);
    QueryPrep::WholeMembershipPlan plan(
        *weight, use, context.filterUses, PreparedDomainDependence::QUERY_CANONICAL,
        QueryPrep::WholeMembershipConsumer::TOP_K_COUNT);
    int64_t result = 0;
    for (auto& segment : reader->segments()) {
      DocSet* domain = segment.liveDocSet();
      auto resolved = test::resolveWholeMembershipForTests(plan, *reader, segment, domain);
      EXPECT_TRUE(resolved.available);
      result += resolved.count;
    }
    return result;
  };

  int64_t plain = count(1.0f);
  auto afterPlain = cache->counters();
  int64_t boosted = count(3.0f);
  auto afterBoosted = cache->counters();
  EXPECT_EQ(plain, boosted);
  EXPECT_EQ(3, plain);
  EXPECT_EQ(1u, cache->entryCountForTest());
  EXPECT_EQ(afterPlain.builds, afterBoosted.builds);
  EXPECT_EQ(afterBoosted.hits, afterPlain.hits);
  EXPECT_EQ(0u, afterPlain.builds);
}

TEST(FilterCacheIntegrationTest, cacheFirstKnnCountOmitsWeightAndPrepare) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_knn_count_first";
  luxir::test::CollectionHelper helper(node, collection);
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;

  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 24; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  std::array<float, 2> queryVector{0.0f, 0.0f};

  struct Run {
    int64_t count;
    int64_t weightSkips;
    int64_t prepares;
  };
  auto run = [&]() {
    auto request = luxir::test::localReq(node.getSearchEngine());
    auto& cursor = request->collection(collection)
                       .topDocs("q").countLimit().limit(0);
    cursor.rawQuery() = luxir::test::qb::knn(
        cursor.mr(), "embedding_v", queryVector, 4, 0,
        /*exact=*/true);
    OwnedFilterStatsGuard stats;
    request->execute(false);
    EXPECT_TRUE(request->ok()) << request->toString();
    return Run{
        request->getMatchCount("q"),
        SkipStats::cacheFirstMembershipWeightSkips,
        KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed)};
  };

  int64_t preparesBefore = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  Run bypass = run();
  Run build = run();
  Run hit = run();
  EXPECT_EQ(4, bypass.count);
  EXPECT_EQ(bypass.count, build.count);
  EXPECT_EQ(build.count, hit.count);
  EXPECT_EQ(preparesBefore + 1, bypass.prepares);
  EXPECT_EQ(bypass.prepares + 1, build.prepares);
  EXPECT_EQ(build.prepares, hit.prepares);
  EXPECT_EQ(0, bypass.weightSkips);
  EXPECT_EQ(0, build.weightSkips);
  EXPECT_EQ(1, hit.weightSkips);
  EXPECT_EQ(1u, cache->counters().readerStableHits);
}

TEST(FilterCacheIntegrationTest,
     nestedCanonicalKnnRetainsReaderStableWholeMembership) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_nested_knn_count";
  luxir::test::CollectionHelper helper(node, collection);
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;

  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 24; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  std::array<float, 2> queryVector{0.0f, 0.0f};

  struct Run {
    int64_t count;
    int64_t prepares;
  };
  auto run = [&]() {
    auto request = luxir::test::localReq(node.getSearchEngine());
    auto& root = request->collection(collection)
                     .topDocs("root").allQuery().limit(0);
    auto& nested = root.topDocs("near").countLimit().limit(0);
    nested.rawQuery() = luxir::test::qb::knn(
        nested.mr(), "embedding_v", queryVector, 4, 0,
        /*exact=*/true);
    request->execute(false);
    EXPECT_TRUE(request->ok()) << request->toString();
    const auto* rootResult = request->docList("root");
    EXPECT_NE(nullptr, rootResult);
    const auto* nestedResult = rootResult == nullptr
        ? nullptr : rootResult->ops.find("near");
    const auto* nestedDocs = nestedResult == nullptr
        ? nullptr : (**nestedResult).docList();
    return Run{
        nestedDocs != nullptr && nestedDocs->found ? *nestedDocs->found : 0,
        KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed)};
  };

  int64_t preparesBefore = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  Run bypass = run();
  Run build = run();
  Run hit = run();
  EXPECT_EQ(4, bypass.count);
  EXPECT_EQ(bypass.count, build.count);
  EXPECT_EQ(build.count, hit.count);
  EXPECT_EQ(preparesBefore + 1, bypass.prepares);
  EXPECT_EQ(bypass.prepares + 1, build.prepares);
  EXPECT_EQ(build.prepares, hit.prepares);
  EXPECT_EQ(1u, cache->counters().readerStableHits);
}

TEST(FilterCacheIntegrationTest, knnReaderValueStaysPinnedDuringRetirement) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_knn_pin";
  luxir::test::CollectionHelper helper(node, collection);
  installVectorSchema(helper.collection());
  FilterCacheConfig cacheConfig = testConfig();
  cacheConfig.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  auto writer = helper.getIndexWriter();
  writer->snapshots.readers.filterCache = cache;

  std::vector<luxir::test::Doc> input;
  input.reserve(4096);
  for (int32_t i = 0; i < 4096; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);

  constexpr int32_t k = 1024;
  std::array<float, 2> queryVector{0.0f, 0.0f};
  auto reader = writer->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  auto* fieldType = dynamic_cast<VectorFieldType*>(
      schema->getFieldTypePtr("embedding_v"));
  ASSERT_NE(nullptr, fieldType);

  KnnQuery knn("embedding_v", *fieldType, queryVector, k, 0, 0, 0.0f,
               /*exact=*/true);
  auto domains = canonicalDomains(*reader);
  {
    MemPool buildPool;
    FilterKeyContext buildKeyContext;
    buildKeyContext.schemaGen = schema->gen_;
    Query::Context buildContext(
        buildPool, *reader, Query::Context::Limits{}, nullptr,
        buildKeyContext);
    auto* buildWeight = knn.createWeight(buildContext, 0);
    auto* buildUse = buildContext.getFilterUse(
        knn, FilterKeyScope::READER_STABLE,
        FilterCache::AdmissionLane::WHOLE);
    QueryPrep::WholeMembershipPlan buildPlan(
        *buildWeight, buildUse, buildContext.filterUses);
    Query::Weight::PrepareContext buildPrepareContext{
        *reader, domains, nullptr, nullptr};
    auto buildPrepared = buildPlan.prepareMainWeight(buildPrepareContext);
    int64_t built = 0;
    for (auto& segment : reader->segments()) {
      auto result = test::resolveWholeMembershipForTests(buildPlan,
          *reader, segment, domains[(size_t)segment.ord],
          buildPrepared.get());
      ASSERT_TRUE(result.available);
      built += result.count;
    }
    ASSERT_EQ(k, built);
  }

  MemPool contextPool;
  FilterKeyContext keyContext;
  keyContext.schemaGen = schema->gen_;
  Query::Context context(
      contextPool, *reader, Query::Context::Limits{}, nullptr, keyContext);
  auto* weight = knn.createWeight(context, 0);
  ASSERT_TRUE(weight->needsPrepare());
  auto* use = context.getFilterUse(
      knn, FilterKeyScope::READER_STABLE,
      FilterCache::AdmissionLane::WHOLE);
  QueryPrep::WholeMembershipPlan plan(
      *weight, use, context.filterUses);
  Query::Weight::PrepareContext prepareContext{
      *reader, domains, nullptr, nullptr};
  uint64_t hitsBefore = cache->counters().readerStableHits;
  auto prepared = plan.prepareMainWeight(prepareContext);
  ASSERT_NE(nullptr, prepared);
  ASSERT_EQ(hitsBefore + 1, cache->counters().readerStableHits);

  std::vector<DomainHandle> cachedMembership;
  cachedMembership.reserve(reader->segments().size());
  int64_t cachedCount = 0;
  for (auto& segment : reader->segments()) {
    auto result = test::resolveWholeMembershipForTests(plan,
        *reader, segment, domains[(size_t)segment.ord], prepared.get());
    ASSERT_TRUE(result.available);
    cachedCount += result.count;
    cachedMembership.push_back(std::move(result.docs));
  }
  ASSERT_EQ(k, cachedCount);
  // Only the plan result's request-lifetime pin remains. Retirement must not
  // invalidate the DocSets after the prepared cache wrapper is destroyed.
  prepared.reset();

  auto collectMembership = [&]() -> int64_t {
    int64_t total = 0;
    for (auto& segment : reader->segments()) {
      MemPool pool;
      auto* scorer = QueryPrep::createDocSetScorer(
          pool, cachedMembership[(size_t)segment.ord].get(), segment);
      if (scorer == nullptr) continue;
      while (scorer->next() != PostingsReader::END) total++;
    }
    return total;
  };

  std::latch collecting(1);
  std::atomic<bool> publicationDone{false};
  std::atomic<bool> collectFailed{false};
  std::atomic<int32_t> rounds{0};
  std::thread collector([&] {
    bool signaled = false;
    try {
      if (collectMembership() != k) collectFailed.store(true);
      rounds.fetch_add(1, std::memory_order_relaxed);
      collecting.count_down();
      signaled = true;
      while (!publicationDone.load(std::memory_order_acquire)) {
        if (collectMembership() != k) collectFailed.store(true);
        rounds.fetch_add(1, std::memory_order_relaxed);
      }
      // These post-retirement passes make the borrowed-DocSet lifetime bug
      // deterministic under ASan while remaining part of the same request.
      for (int i = 0; i < 8; i++) {
        if (collectMembership() != k) collectFailed.store(true);
        rounds.fetch_add(1, std::memory_order_relaxed);
      }
    } catch (...) {
      collectFailed.store(true);
      if (!signaled) collecting.count_down();
    }
  });

  collecting.wait();
  luxir::test::CollectionHelper::UpdateBuilder update;
  update.remove("0").commit();
  auto updateResult = helper.submit(update);
  std::shared_ptr<IndexReader> nextReader;
  bool readerFailed = false;
  try {
    nextReader = writer->snapshots.readers.getReader();
  } catch (...) {
    readerFailed = true;
  }
  publicationDone.store(true, std::memory_order_release);
  collector.join();

  ASSERT_TRUE(updateResult.success) << updateResult.error_message;
  ASSERT_FALSE(readerFailed);
  ASSERT_NE(nullptr, nextReader);
  EXPECT_GT(nextReader->commitTime(), reader->commitTime());
  EXPECT_GT(cache->counters().readerStableRetires, 0u);
  EXPECT_FALSE(collectFailed.load());
  EXPECT_GT(rounds.load(), 8);
  ASSERT_NO_THROW(cache->validateForTest());
}

// A kNN filter under a facet bucket prepares against each bucket's domain, so
// its reader-stable value must neither be served nor sighted for admission.
TEST(FilterCacheIntegrationTest, nestedBucketKnnDoesNotRecordAdmission) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_knn_gate");
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;

  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 20; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i), "group_s", (i & 1) == 0 ? "a" : "b",
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  std::array<float, 2> queryVector{0.0f, 0.0f};

  auto runNested = [&]() {
    auto request = luxir::test::localReq(node.getSearchEngine());
    auto& top = request->collection("filter_cache_knn_gate")
                    .topDocs("q").allQuery().limit(0);
    auto& nested = top.facet("groups", "group_s").limit(-1)
                       .topDocs("near").allQuery().limit(-1);
    addFilter(nested, luxir::test::qb::knn(
        nested.mr(), "embedding_v", queryVector, 3, 0, true));
    request->execute();
    ASSERT_TRUE(request->ok()) << request->toString();
    // Each bucket keeps its own three nearest of its ten documents.
    const auto* groups = request->docList("q")->ops.at("groups")->facetResult();
    ASSERT_NE(nullptr, groups);
    const auto& near = std::get<luxir::api::ArrVal>(
        groups->ops.at("near")->kind).v;
    ASSERT_EQ(2u, near.size());
    for (const auto& bucket : near) {
      ASSERT_NE(nullptr, bucket.docList());
      EXPECT_EQ(3, bucket.docList()->row_count);
    }
  };

  runNested();
  runNested();
  auto nestedCounters = cache->counters();
  EXPECT_EQ(0u, nestedCounters.admissions);
  EXPECT_EQ(0u, nestedCounters.builds);
  EXPECT_EQ(0u, cache->entryCountForTest());

  runKnnFilter(node, "filter_cache_knn_gate", queryVector, 3);
  EXPECT_EQ(0u, cache->counters().admissions)
      << "nested bucket requests must not count as an admission sighting";
  runKnnFilter(node, "filter_cache_knn_gate", queryVector, 3);
  auto built = cache->counters();
  EXPECT_EQ(1u, built.admissions);
  EXPECT_EQ(1u, built.builds);
  runKnnFilter(node, "filter_cache_knn_gate", queryVector, 3);
  EXPECT_EQ(1u, cache->counters().readerStableHits);
}

TEST(FilterCacheIntegrationTest, cachedArrayComposesWithDeletedLiveDocs) {
  // An ArrDocSet entry is by construction below the scored-route density
  // crossover, so composition is observed through the op-level domain path,
  // which serves cached entries at any density.
  struct FoldGuard {
    bool saved = disableTopDocsFilterFold;
    FoldGuard() { disableTopDocsFilterFold = true; }
    ~FoldGuard() { disableTopDocsFilterFold = saved; }
  } foldGuard;
  LuxirConfig config;
  config.queryCacheBytes = 4 * 1024 * 1024;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_mixed");
  std::vector<luxir::test::Doc> input;
  input.reserve(1100);
  for (int32_t i = 0; i < 1100; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i), "body_w", "body",
        "filter_w", i < 30 ? "sparse" : "other"));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  auto cache = helper.getIndexWriter()->snapshots.readers.filterCache;
  EXPECT_EQ(30, runCachedSearch(node, "filter_cache_mixed", "sparse").count);
  EXPECT_EQ(30, runCachedSearch(node, "filter_cache_mixed", "sparse").count);

  luxir::test::CollectionHelper::UpdateBuilder update;
  for (int32_t i = 0; i < 20; i++) update.remove(std::to_string(i));
  update.commit();
  ASSERT_TRUE(helper.submit(update).success);
  auto before = cache->counters();

  EXPECT_EQ(10, runCachedSearch(node, "filter_cache_mixed", "sparse").count);
  EXPECT_GT(cache->counters().hits, before.hits);
}

TEST(FilterCacheIntegrationTest, multiSelectFacetExactDomainWarmsSources) {
  LuxirConfig config;
  config.queryCacheBytes = 4 * 1024 * 1024;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_facet");
  std::vector<luxir::test::Doc> docs;
  docs.reserve(1100);
  for (int32_t i = 0; i < 1100; i++) {
    docs.push_back(luxir::test::flatdoc(
        "id", std::to_string(i), "body_w", "body",
        "filter_w", (i & 1) == 0 ? "keep" : "drop",
        "group_s", (i % 3) == 0 ? "a" : "b"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto run = [&]() {
    auto request = luxir::test::localReq(node.getSearchEngine());
    auto& top = request->collection("filter_cache_facet")
                    .topDocs("q")
                    .matchQuery("body_w", "body")
                    .matchFilter("filter_w", "keep")
                    .matchFilter("group_s", "a")
                    .countLimit()
                    .limit(0);
    top.facet("groups", "group_s").limit(-1);
    FilterFoldGuard passive(true);
    request->execute();
    EXPECT_TRUE(request->ok()) << request->toString();
    return request->getMatchCount("q");
  };

  EXPECT_EQ(184, run());
  auto beforeBuild = helper.getIndexWriter()->snapshots.readers.filterCache->counters();
  EXPECT_EQ(184, run());
  auto afterBuild = helper.getIndexWriter()->snapshots.readers.filterCache->counters();
  EXPECT_GE(afterBuild.builds - beforeBuild.builds, 2u);
  auto beforeHit = afterBuild;
  EXPECT_EQ(184, run());
  EXPECT_GE(helper.getIndexWriter()->snapshots.readers.filterCache->counters().hits
                - beforeHit.hits,
            2u);
}

TEST(DocSetRamBytesTest, usesOwnedCapacityForBothRepresentations) {
  std::vector<int32_t> values;
  values.reserve(100);
  values.push_back(1);
  ArrDocSet array(std::move(values));
  EXPECT_GE(array.ramBytesUsed(), sizeof(ArrDocSet) + 100 * sizeof(int32_t));

  RAMBitDocSet bits(1024);
  EXPECT_EQ(sizeof(RAMBitDocSet) + 16 * sizeof(uint64_t), bits.ramBytesUsed());
}

namespace {

FilterKey keyFor(const Query& query, FilterKeyContext ctx = {}) {
  FilterKeyBuilder builder;
  FilterKeyScope scope = query.appendFilterKey(builder, ctx);
  auto key = std::move(builder).finish(scope, ctx);
  if (!key) throw std::runtime_error("query unexpectedly uncacheable");
  return std::move(*key);
}

std::optional<FilterKey> optionalKeyFor(const Query& query,
                                         FilterKeyContext ctx = {}) {
  FilterKeyBuilder builder;
  FilterKeyScope scope = query.appendFilterKey(builder, ctx);
  return std::move(builder).finish(scope, ctx);
}

FilterKey floatKey(uint32_t bits) {
  FilterKeyBuilder builder;
  builder.appendKind(QueryKind::CONSTANT_SCORE);
  builder.appendFloat(std::bit_cast<float>(bits));
  return std::move(builder)
      .finish(FilterKeyScope::SEGMENT_STABLE, {})
      .value();
}

class KeyOnlyTermQuery final : public Query {
  std::string_view term;

public:
  explicit KeyOnlyTermQuery(std::string_view term)
    : Query(QueryKind::TEST), term(term) {}

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendTerm(term);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    unused(context);
    unused(flags);
    unused(multiplier);
    return nullptr;
  }
};

} // namespace

TEST(FilterKeyTest, discriminatesFieldsValuesAndTermLengthFraming) {
  TermQuery base("ab", "c");
  TermQuery field("xy", "c");
  TermQuery value("ab", "d");
  TermQuery ambiguousConcat("a", "bc");

  std::array<std::string_view, 2> firstTerms{"ab", "c"};
  std::array<std::string_view, 2> secondTerms{"a", "bc"};
  std::array<int32_t, 2> positions{0, 1};
  PhraseQuery firstPhrase("f", firstTerms, positions);
  PhraseQuery secondPhrase("f", secondTerms, positions);

  EXPECT_NE(keyFor(base), keyFor(field));
  EXPECT_NE(keyFor(base), keyFor(value));
  EXPECT_NE(keyFor(base), keyFor(ambiguousConcat));
  EXPECT_NE(keyFor(firstPhrase), keyFor(secondPhrase));
  EXPECT_EQ(keyFor(base), keyFor(TermQuery("ab", "c")));
}

TEST(FilterKeyTest, usesVIntZigzagAndPackedTermWireShapes) {
  FilterKeyBuilder builder;
  builder.appendUInt32(127);
  builder.appendUInt32(128);
  builder.appendUInt64(16384);
  builder.appendInt32(-1);
  builder.appendInt32(std::numeric_limits<int32_t>::max());
  builder.appendInt64(-1);
  builder.appendInt64((int64_t)1 << 40);
  builder.appendTerm("ab");
  FilterKey key = std::move(builder)
      .finish(FilterKeyScope::SEGMENT_STABLE, {})
      .value();

  std::vector<std::byte> expected{
      std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0x7f}, std::byte{0x80}, std::byte{0x01},
      std::byte{0x80}, std::byte{0x80}, std::byte{0x01},
      std::byte{0x01},
      std::byte{0xfe}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
      std::byte{0x0f},
      std::byte{0x01},
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
      std::byte{0x80}, std::byte{0x40},
      std::byte{2}, std::byte{'a'}, std::byte{'b'}};
  EXPECT_EQ(expected, key.bytes());
  EXPECT_EQ(Hash::hash(key.bytes().data(), key.bytes().size()), key.hash());
}

TEST(FilterKeyTest, zigzagPreventsCrossWidthSignedCollisions) {
  FilterKeyBuilder negativeInt32;
  negativeInt32.appendInt32(-1);
  negativeInt32.appendInt64(7);

  FilterKeyBuilder largePositiveInt64;
  largePositiveInt64.appendInt64(
      (int64_t)std::numeric_limits<uint32_t>::max());
  largePositiveInt64.appendInt32(7);

  auto first = std::move(negativeInt32)
      .finish(FilterKeyScope::SEGMENT_STABLE, {})
      .value();
  auto second = std::move(largePositiveInt64)
      .finish(FilterKeyScope::SEGMENT_STABLE, {})
      .value();
  EXPECT_NE(first, second);
}

TEST(FilterKeyTest, booleanShortTermFramingStaysCompact) {
  constexpr size_t TERM_COUNT = 1000;
  KeyOnlyTermQuery term("x");
  std::vector<Query*> clauses(TERM_COUNT, &term);
  BooleanQuery boolean({}, clauses, {}, {});

  size_t termPayloadAndFraming = TERM_COUNT * (1 + 2);
  size_t keySize = keyFor(boolean).bytes().size();
  EXPECT_EQ(termPayloadAndFraming + 14, keySize);
}

TEST(FilterKeyTest, discriminatesBooleanRolesMultiplicityMinMatchAndShape) {
  TermQuery a("f", "a");
  TermQuery b("f", "b");
  std::array<Query*, 1> oneA{&a};
  std::array<Query*, 1> oneB{&b};
  std::array<Query*, 2> twoA{&a, &a};

  BooleanQuery mandatory(oneA, {}, {}, {});
  BooleanQuery optional({}, oneA, {}, {}, 1);
  BooleanQuery prohibited({}, {}, oneA, {});
  BooleanQuery filter({}, {}, {}, oneA);
  BooleanQuery duplicate(twoA, {}, {}, {});
  BooleanQuery minOne({}, oneA, {}, {}, 1);
  BooleanQuery minZero({}, oneA, {}, {}, 0);
  BooleanQuery inner(oneA, {}, {}, {});
  std::array<Query*, 1> innerClause{&inner};
  BooleanQuery nested(innerClause, {}, {}, {});
  BooleanQuery flat(oneA, {}, {}, {});
  BooleanQuery differentValue(oneB, {}, {}, {});

  EXPECT_NE(keyFor(mandatory), keyFor(optional));
  EXPECT_NE(keyFor(mandatory), keyFor(prohibited));
  EXPECT_NE(keyFor(mandatory), keyFor(filter));
  EXPECT_NE(keyFor(mandatory), keyFor(duplicate));
  EXPECT_NE(keyFor(minOne), keyFor(minZero));
  EXPECT_NE(keyFor(nested), keyFor(flat));
  EXPECT_NE(keyFor(mandatory), keyFor(differentValue));
}

TEST(FilterKeyTest, booleanOptionalsEncodeOnlyWhenTheyAffectMembership) {
  TermQuery a("f", "a");
  TermQuery b("f", "b");
  std::array<Query*, 1> oneA{&a};
  std::array<Query*, 1> oneB{&b};
  std::array<Query*, 2> aAndB{&a, &b};

  BooleanQuery mandatory(oneA, {}, {}, {});
  BooleanQuery scoreOnlyOptional(oneA, oneB, {}, {});
  EXPECT_EQ(keyFor(mandatory), keyFor(scoreOnlyOptional));

  BooleanQuery mandatoryMinOne(oneA, {}, {}, {}, 1);
  BooleanQuery requiredOptional(oneA, oneB, {}, {}, 1);
  EXPECT_NE(keyFor(mandatoryMinOne), keyFor(requiredOptional));

  BooleanQuery oneOptional({}, oneA, {}, {});
  BooleanQuery twoOptionals({}, aAndB, {}, {});
  EXPECT_NE(keyFor(oneOptional), keyFor(twoOptionals));
}

TEST(FilterKeyTest, termScoreAndExecutionStateStaysOutOfMembershipKey) {
  TermQuery plain("body_w", "alpha");
  TermQuery boosted("body_w", "alpha", 2.5f);
  TermQuery noFrontier("body_w", "alpha", 1.0f, /*useFrontierBound=*/false);
  TermQuery injected("body_w", "alpha", Similarity::TermStats{123, 456});
  EXPECT_EQ(keyFor(plain, 7), keyFor(boosted, 7));
  EXPECT_EQ(keyFor(plain, 7), keyFor(noFrontier, 7));
  EXPECT_EQ(keyFor(plain, 7), keyFor(injected, 7));
  TermQuery otherTerm("body_w", "beta");
  EXPECT_FALSE(keyFor(plain, 7) == keyFor(otherTerm, 7));
}

TEST(FilterKeyTest, scoreAndExecutionWrappersPreserveMembershipKey) {
  TermQuery term("f", "v");
  BoostQuery boost(&term, 2.0f);
  ConstantScoreQuery constant(&term, 5.0f);
  ForcePrepareQuery force(&term);

  EXPECT_EQ(keyFor(term), keyFor(boost));
  EXPECT_EQ(keyFor(term), keyFor(constant));
  EXPECT_EQ(keyFor(term), keyFor(force));
}

TEST(FilterKeyTest, requestScopePropagatesSeparatelyFromQueryEquality) {
  NumericPredicateQuery request("when_dt", 1, 2, FilterKeyScope::REQUEST_STABLE);
  NumericPredicateQuery literal("when_dt", 1, 2);
  // BooleanQuery dedups membership by equals(), independently of cache lifetime.
  EXPECT_TRUE(request.equals(literal));
  EXPECT_EQ(request.hash(), literal.hash());
  EXPECT_NE(keyFor(request), keyFor(literal));
  std::array<Query*, 2> clauses{&literal, &request};
  BooleanQuery compound(clauses, {}, {}, {});
  FilterKeyBuilder out;
  EXPECT_EQ(FilterKeyScope::REQUEST_STABLE, compound.appendFilterKey(out, {}));
  UncacheableQuery uncacheable(request);
  std::array<Query*, 2> mixed{&request, &uncacheable};
  BooleanQuery rejected(mixed, {}, {}, {});
  EXPECT_FALSE(optionalKeyFor(rejected));
}

// An unrounded NOW range must not touch admission history, so a rounded filter's
// first sighting survives a stream of them and admits on its second.
TEST(FilterCacheIntegrationTest, unroundedNowRangeSkipsAdmission) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_unrounded_now";
  luxir::test::CollectionHelper helper(node, collection);
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  // Column only: a points index would answer the count without a filter.
  SchemaBuilder schema;
  auto& when = schema.field("when_dt");
  when.type = api::FieldDef_::FieldClass::DATE;
  when.index = api::FieldDef_::IndexMode::NONE;
  schema.set(helper.collection());

  constexpr int64_t minute = 60000;
  int64_t base = *parseDateToEpochMillis("2026-07-22T08:00:00Z");
  std::vector<luxir::test::Doc> input;
  for (int64_t i = 0; i < 60; i++) {
    input.push_back(luxir::test::flatdoc("when_dt", base + i * minute));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);

  struct NowRestore {
    ~NowRestore() { forcedDateMathNowEpochMillis.reset(); }
  } restore;
  auto count = [&](int64_t now, std::string_view gte, std::string_view lte) {
    forcedDateMathNowEpochMillis = now;
    auto request = luxir::test::localReq(node.getSearchEngine());
    auto& cursor = request->collection(collection)
                       .topDocs("q").allQuery().countLimit().limit(1);
    addFilter(cursor, luxir::test::qb::range(
        cursor.mr(), "when_dt", luxir::test::qb::valStr(cursor.mr(), gte),
        nullptr, luxir::test::qb::valStr(cursor.mr(), lte), nullptr));
    request->execute();
    EXPECT_TRUE(request->ok()) << request->toString();
    return request->getMatchCount("q");
  };

  int64_t start = base + 30 * minute + 500;
  EXPECT_EQ(11, count(start, "now-10m/m", "now/m"));  // first sighting
  // More unique keys than testConfig's admission history holds.
  for (int64_t k = 0; k < 40; k++) {
    EXPECT_EQ(10, count(start + k, "now-10m", "now"));
  }
  EXPECT_EQ(0u, cache->entryCountForTest());
  EXPECT_EQ(0u, cache->counters().builds);

  EXPECT_EQ(11, count(start, "now-10m/m", "now/m"));
  EXPECT_GT(cache->counters().builds, 0u);
  auto hitsBefore = cache->counters().hits;
  EXPECT_EQ(11, count(start + 1000, "now-10m/m", "now/m"));
  EXPECT_GT(cache->counters().hits, hitsBefore);
}

TEST(FilterKeyTest, dateMathKeysFollowCoercedBuckets) {
  TimeZone utc = TimeZone::utc();
  int64_t morning = *parseDateToEpochMillis("2026-07-22T08:00:00Z");
  int64_t evening = *parseDateToEpochMillis("2026-07-22T22:00:00Z");
  int64_t tomorrow = *parseDateToEpochMillis("2026-07-23T08:00:00Z");
  DateRange first = *parseDateRange("NOW/DAY", morning, utc);
  DateRange same = *parseDateRange("NOW/DAY", evening, utc);
  DateRange next = *parseDateRange("NOW/DAY", tomorrow, utc);

  NumericPredicateQuery firstQuery("when", first.lo, first.hiExclusive - 1);
  NumericPredicateQuery sameQuery("when", same.lo, same.hiExclusive - 1);
  NumericPredicateQuery nextQuery("when", next.lo, next.hiExclusive - 1);
  FilterKeyContext ctx{.schemaGen = 7, .timeZone = "UTC"};

  EXPECT_EQ(keyFor(firstQuery, ctx), keyFor(sameQuery, ctx));
  EXPECT_NE(keyFor(firstQuery, ctx), keyFor(nextQuery, ctx));
}

TEST(FilterKeyTest, discriminatesTimezoneAndSchemaGeneration) {
  NumericPredicateQuery query("when", 100, 200);
  FilterKeyContext utc{.schemaGen = 3, .timeZone = "UTC"};
  FilterKeyContext newYork{.schemaGen = 3, .timeZone = "America/New_York"};
  FilterKeyContext newSchema{.schemaGen = 4, .timeZone = "UTC"};

  EXPECT_NE(keyFor(query, utc), keyFor(query, newYork));
  EXPECT_NE(keyFor(query, utc), keyFor(query, newSchema));
}

TEST(FilterKeyTest, preservesNonFiniteFloatBitPatterns) {
  uint32_t quietNan = 0x7fc00001U;
  uint32_t otherNan = 0x7fc00002U;
  EXPECT_NE(floatKey(quietNan), floatKey(otherNan));
  EXPECT_NE(floatKey(0x7f800000U), floatKey(0xff800000U));
  EXPECT_NE(floatKey(0x00000000U), floatKey(0x80000000U));
}

TEST(FilterKeyTest, knnDiscriminatesMembershipAndExecutionPolicies) {
  luxir::test::SearchOverridesGuard routing(KnnQuery::tuning.routing, KnnQuery::tuning.plannerCosts);
  struct PolicyRestore {
    int64_t candidates = KnnQuery::maxKnnCandidates;
    double alpha = KnnQuery::tuning.filteredCoverageAlpha, headroom = KnnQuery::tuning.breadthHeadroom, growth = KnnQuery::tuning.breadthGrowth;
    decltype(KnnQuery::engineWrapperForTests) wrapper =
        KnnQuery::engineWrapperForTests;
    ~PolicyRestore() {
      KnnQuery::maxKnnCandidates = candidates;
      KnnQuery::tuning.filteredCoverageAlpha = alpha;
      KnnQuery::tuning.breadthHeadroom = headroom;
      KnnQuery::tuning.breadthGrowth = growth;
      KnnQuery::engineWrapperForTests = std::move(wrapper);
    }
  } restore;

  VectorFieldType vectorType(
      "vec", 2, FieldType::COLUMN_STORED | FieldType::FIXED_SIZE,
      VectorFieldType::METRIC_L2);
  std::array<float, 2> vector{1.0f, 0.0f};
  KnnQuery base("vec", vectorType, vector, 7, 3, 11, 0.25f, false);
  FilterKey baseKey = keyFor(base);
  FilterKeyBuilder scopeBuilder;
  EXPECT_EQ(FilterKeyScope::READER_STABLE,
            base.appendFilterKey(scopeBuilder, {}));

  std::array<float, 2> vectorBits{1.0f, -0.0f};
  std::array<float, 3> vectorLength{1.0f, 0.0f, 0.0f};
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "vec", vectorType, vectorBits, 7, 3, 11, 0.25f, false)));
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "vec", vectorType, vectorLength, 7, 3, 11, 0.25f, false)));
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "other", vectorType, vector, 7, 3, 11, 0.25f, false)));
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "vec", vectorType, vector, 8, 3, 11, 0.25f, false)));
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "vec", vectorType, vector, 7, 4, 11, 0.25f, false)));
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "vec", vectorType, vector, 7, 3, 12, 0.25f, false)));
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "vec", vectorType, vector, 7, 3, 11, 0.5f, false)));
  EXPECT_NE(baseKey, keyFor(KnnQuery(
      "vec", vectorType, vector, 7, 3, 11, 0.25f, true)));

  KnnQuery::tuning.routing = KnnQuery::Routing::FORCE_IVF;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::tuning.routing = KnnQuery::Routing::AUTO;
  KnnQuery::tuning.plannerCosts.units[KnnPlanner::EXACT_DIM]++;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::tuning.plannerCosts.units[KnnPlanner::EXACT_DIM]--;
  EXPECT_EQ(baseKey, keyFor(base));
  KnnQuery::maxKnnCandidates++;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::maxKnnCandidates = restore.candidates;
  KnnQuery::tuning.filteredCoverageAlpha += 0.1;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::tuning.filteredCoverageAlpha = restore.alpha;
  KnnQuery::tuning.breadthHeadroom += 0.1;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::tuning.breadthHeadroom = restore.headroom;
  KnnQuery::tuning.breadthGrowth += 0.1;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::tuning.breadthGrowth = restore.growth;

  KnnQuery::engineWrapperForTests = [](VectorEngine&) {
    return std::unique_ptr<VectorEngine>();
  };
  EXPECT_FALSE(optionalKeyFor(base).has_value());
}

TEST(FilterKeyTest, fuzzyIsCoreStableAndIncludesEffectiveLimit) {
  FuzzyQuery fuzzy("title", "luxir", 2, 1, 0);
  FuzzyQuery boostedFuzzy("title", "luxir", 2, 1, 0, 3.0f);
  BoostQuery wrappedFuzzy(&fuzzy, 3.0f);
  FuzzyQuery explicitDefault("title", "luxir", 2, 1, FuzzyQuery::DEFAULT_MAX_EXPANSIONS);
  FuzzyQuery wide("title", "luxir", 2, 1, 1000);
  FuzzyQuery wider("title", "luxir", 2, 1, 1001);
  FilterKeyContext first{.schemaGen = 5, .coreGen = 10, .timeZone = "UTC"};
  FilterKeyContext otherCore = first;
  otherCore.coreGen = 11;

  EXPECT_NE(keyFor(fuzzy, first), keyFor(fuzzy, otherCore));
  EXPECT_EQ(keyFor(fuzzy, first), keyFor(explicitDefault, first));
  EXPECT_NE(keyFor(wide, first), keyFor(wider, first));
  EXPECT_EQ(keyFor(fuzzy, first), keyFor(boostedFuzzy, first));
  EXPECT_EQ(keyFor(fuzzy, first), keyFor(wrappedFuzzy, first));
}

TEST(FilterKeyTest, everyConcreteQueryMakesAnExplicitScopeDecision) {
  AllQuery all;
  MatchNoDocsQuery none;
  TermQuery term("f", "v");
  std::array<std::string_view, 2> phraseTerms{"a", "b"};
  std::array<int32_t, 2> phrasePositions{0, 1};
  PhraseQuery phrase("f", phraseTerms, phrasePositions, 0);
  ExistsQuery exists("f");
  NumericPredicateQuery numeric("n", 1, 2);
  PrefixQuery prefix("f", "pre");
  TermRangeQuery range("f", "a", true, "z", false);
  FuzzyQuery fuzzy("f", "term", 1);
  GeoBoxQuery box("geo", -1.0, 1.0, -2.0, 2.0);
  GeoDistanceQuery distance("geo", 0.0, 0.0, 1000.0);
  ConstantScoreQuery constant(&term, 2.0f);
  BoostQuery boost(&term, 2.0f);
  ForcePrepareQuery force(&term);
  std::array<Query*, 1> clause{&term};
  BooleanQuery boolean({}, {}, {}, clause);

  for (Query* query : std::array<Query*, 15>{
         &all, &none, &term, &phrase, &exists, &numeric, &prefix, &range,
         &fuzzy, &box, &distance, &constant, &boost, &force, &boolean}) {
    EXPECT_TRUE(optionalKeyFor(*query).has_value());
  }

  VectorFieldType vectorType("vec", 2, FieldType::COLUMN_STORED | FieldType::FIXED_SIZE,
                             VectorFieldType::METRIC_L2);
  std::array<float, 2> vector{1.0f, 2.0f};
  KnnQuery knn("vec", vectorType, vector, 1);
  EXPECT_TRUE(optionalKeyFor(knn).has_value());
  std::array<Query*, 1> knnClause{&knn};
  BooleanQuery withKnn({}, {}, {}, knnClause);
  EXPECT_TRUE(optionalKeyFor(withKnn).has_value());
  std::array<Query*, 1> termClause{&term};
  BooleanQuery scoreOnlyKnn(termClause, knnClause, {}, {});
  EXPECT_TRUE(optionalKeyFor(scoreOnlyKnn).has_value());
  BooleanQuery disjunctionWithKnn({}, knnClause, {}, {});
  EXPECT_TRUE(optionalKeyFor(disjunctionWithKnn).has_value());
}

TEST(FilterKeyTest, fullBytesDefeatForcedHashCollision) {
  FilterKey first = FilterKey::withHashForTest("first", 17);
  FilterKey second = FilterKey::withHashForTest("second", 17);
  EXPECT_EQ(first.hash(), second.hash());
  EXPECT_NE(first, second);
}
using namespace luxir::test;

namespace {

constexpr int32_t DOC_UNIVERSE = 96;

enum class FilterKind : uint8_t {
  DENSE_TERM,
  SPARSE_TERM,
  NUMERIC_PREDICATE_RANGE,
  REQUIRED_AND_FILTER,
  REQUIRED_AND_OPTIONAL,
  MATCH_NONE,
  PREFIX_BROAD,
  PREFIX_NARROW,
  COUNT
};

constexpr std::array<std::string_view, (size_t)FilterKind::COUNT> FILTER_NAMES{
    "dense-term", "sparse-term", "numeric-range", "required-and-filter",
    "required-and-optional", "match-none", "prefix-p0", "prefix-p00"};

std::string_view filterName(FilterKind kind) {
  return FILTER_NAMES[(size_t)kind];
}

bool matchesFilter(FilterKind kind, int32_t id) {
  switch (kind) {
    case FilterKind::DENSE_TERM:
      return id % 3 == 0;
    case FilterKind::SPARSE_TERM:
      return id % 17 == 0;
    case FilterKind::NUMERIC_PREDICATE_RANGE: {
      int32_t value = id % 19;
      return value >= 5 && value <= 11;
    }
    case FilterKind::REQUIRED_AND_FILTER:
      return id % 2 == 0 && id % 3 == 0;
    case FilterKind::REQUIRED_AND_OPTIONAL:
      // The optional clause has msm=0 and is score-only. Its membership key
      // must therefore be the same as the required dense term.
      return id % 3 == 0;
    case FilterKind::MATCH_NONE:
      return false;
    case FilterKind::PREFIX_BROAD:
      return id % 2 == 0;
    case FilterKind::PREFIX_NARROW:
      return id % 4 == 0;
    case FilterKind::COUNT:
      break;
  }
  return false;
}

api::Query buildFilter(FilterKind kind, std::pmr::memory_resource& mr) {
  switch (kind) {
    case FilterKind::DENSE_TERM:
      return qb::match(mr, "tag_s", "t0");
    case FilterKind::SPARSE_TERM:
      return qb::match(mr, "sparse_s", "rare");
    case FilterKind::NUMERIC_PREDICATE_RANGE:
      return qb::range(mr, "num_i", qb::valI64(mr, 5), nullptr,
                       qb::valI64(mr, 11), nullptr);
    case FilterKind::REQUIRED_AND_FILTER: {
      auto required = qb::match(mr, "body_w", "even");
      auto filter = qb::match(mr, "tag_s", "t0");
      return qb::boolean(mr, {required}, {}, {}, {filter});
    }
    case FilterKind::REQUIRED_AND_OPTIONAL: {
      auto required = qb::match(mr, "tag_s", "t0");
      auto optional = qb::match(mr, "body_w", "bonus");
      return qb::boolean(mr, {required}, {optional}, {}, {}, 0);
    }
    case FilterKind::MATCH_NONE:
      return qb::match(mr, "tag_s", "never-indexed");
    case FilterKind::PREFIX_BROAD:
      return qb::prefix(mr, "prefix_s", "p0");
    case FilterKind::PREFIX_NARROW:
      return qb::prefix(mr, "prefix_s", "p00");
    case FilterKind::COUNT:
      break;
  }
  return qb::all();
}

void addConcurrencyFilter(OpCursor& cursor, const api::Query& query) {
  auto& top = std::get<api::TopDocs>(cursor.rawOp().kind);
  auto* filter = api::build::allocArray(top.filter, 1, cursor.mr());
  auto* stored = (api::Query*)cursor.mr().allocate(sizeof(api::Query),
                                                    alignof(api::Query));
  new (stored) api::Query(query);
  filter[0].query = stored;
}

Doc docFor(int32_t id) {
  std::string body = "common";
  if (id % 2 == 0) body += " even";
  if (id % 5 == 0) body += " bonus";
  std::string prefix = id % 4 == 0 ? "p00-leaf"
      : id % 2 == 0 ? "p01-leaf" : "q00-leaf";
  return flatdoc(
      "id", std::to_string(id), "body_w", body,
      "tag_s", id % 3 == 0 ? "t0" : "t1",
      "sparse_s", id % 17 == 0 ? "rare" : "ordinary",
      "num_i", (int64_t)(id % 19), "prefix_s", prefix);
}

struct SearchResult {
  bool valid = false;
  int64_t count = 0;
  std::vector<int32_t> ids;
  std::string error;
};

SearchResult runSearch(SearchEngine& engine, std::string_view collection,
                       std::optional<FilterKind> filter) {
  auto request = localReq(engine);
  auto& cursor = request->collection(collection)
                     .topDocs("q")
                     .allQuery()
                     .fields({"id"})
                     .countLimit()
                     .limit(DOC_UNIVERSE + 1);
  if (filter.has_value()) {
    addConcurrencyFilter(cursor, buildFilter(*filter, cursor.mr()));
  }
  request->execute();

  SearchResult result;
  if (!request->ok()) {
    result.error = "search failed: " + request->errorMsg();
    return result;
  }
  auto docs = request->getDocs("q");
  result.count = request->getMatchCount("q");
  if (result.count != (int64_t)docs.size()) {
    std::ostringstream out;
    out << "count oracle: count_limit=" << result.count
        << " returned=" << docs.size();
    if (filter.has_value()) out << " filter=" << filterName(*filter);
    result.error = out.str();
    return result;
  }

  result.ids.reserve(docs.size());
  for (const auto& doc : docs) {
    const FieldVal* value = find(doc, "id");
    const auto* text = value == nullptr
        ? nullptr : std::get_if<std::string>(value);
    int32_t id = -1;
    if (text == nullptr) {
      result.error = "membership oracle: response id is absent or non-string";
      return result;
    }
    auto parsed = std::from_chars(text->data(), text->data() + text->size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size()
        || id < 0 || id >= DOC_UNIVERSE) {
      result.error = "membership oracle: invalid response id " + *text;
      return result;
    }
    if (filter.has_value() && !matchesFilter(*filter, id)) {
      std::ostringstream out;
      out << "arithmetic membership oracle: filter=" << filterName(*filter)
          << " returned id=" << id;
      result.error = out.str();
      return result;
    }
    result.ids.push_back(id);
  }
  std::sort(result.ids.begin(), result.ids.end());
  result.valid = true;
  return result;
}

struct FailureState {
  std::atomic<bool> failed{false};
  std::mutex mutex;
  std::string firstFailure;

  void record(std::string message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true,
                                       std::memory_order_relaxed)) {
      std::lock_guard<std::mutex> lock(mutex);
      firstFailure = std::move(message);
    }
  }
};

IndexResult commitAndDrain(CollectionHelper& helper) {
  CollectionHelper::UpdateBuilder commit;
  commit.commit(true);
  auto result = helper.submit(commit);
  helper.getIndexWriter()->updateGraph->wait_for_all();
  helper.getIndexWriter()->snapshots.readers.getReader();
  return result;
}

void validateConcurrentByproductPublication() {
  constexpr int threadCount = 16;
  constexpr int rounds = 8;
  FilterCacheConfig config{
      .maxBytes = 1024 * 1024,
      .maxEntryBytes = 128 * 1024,
      .minSegmentDocs = 0,
      .admissionHistorySize = 64,
      .admissionThreshold = 1,
      .maxMetadataEntries = 64,
      .maxMetadataBytes = 128 * 1024};
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 256}};
  cache.onReaderPublished(1, segments);

  std::vector<FilterKey> keys;
  keys.reserve(rounds);
  for (int round = 0; round < rounds; round++) {
    keys.emplace_back("byproduct-race-" + std::to_string(round));
    FilterCache::UseRegistry prepare(cache, 1, segments);
    prepare.get(keys.back());
  }

  std::barrier phase(threadCount + 1);
  std::vector<std::thread> threads;
  for (int tid = 0; tid < threadCount; tid++) {
    threads.emplace_back([&, tid] {
      for (int round = 0; round < rounds; round++) {
        FilterCache::UseRegistry request(cache, 1, segments);
        auto* use = request.get(keys[round]);
        DocSetBuilder builder(256);
        builder.add(tid + 1);
        auto raw = builder.build();
        phase.arrive_and_wait();
        use->offerRaw(0, std::move(raw), 1);
        phase.arrive_and_wait();
        phase.arrive_and_wait();
      }
    });
  }

  std::string validationFailure;
  for (int round = 0; round < rounds; round++) {
    phase.arrive_and_wait();
    phase.arrive_and_wait();
    try {
      cache.validateForTest();
    } catch (const std::exception& e) {
      if (validationFailure.empty()) validationFailure = e.what();
    }
    phase.arrive_and_wait();
  }
  for (auto& thread : threads) thread.join();
  if (!validationFailure.empty()) {
    throw std::logic_error(validationFailure);
  }
  if (cache.counters().byproductInserts != rounds) {
    throw std::logic_error(
        "FilterCache validation: byproduct publication was not first-wins");
  }
  cache.clear();
  cache.validateForTest();
}

void validateStalePublicationRejection() {
  FilterCacheConfig config{
      .maxBytes = 1024 * 1024,
      .maxEntryBytes = 128 * 1024,
      .minSegmentDocs = 0,
      .admissionHistorySize = 8,
      .admissionThreshold = 1,
      .maxMetadataEntries = 8,
      .maxMetadataBytes = 128 * 1024};
  FilterCache cache(config);
  std::array oldSegments{FilterCache::SegmentIdentity{1, 256}};
  cache.onReaderPublished(7, oldSegments);
  FilterCache::UseRegistry staleRequest(cache, 7, oldSegments);
  auto* use = staleRequest.get(FilterKey("stale-publication"));
  auto claim = use->probe(0);
  if (claim.kind() != FilterCache::Probe::Kind::BUILD) {
    throw std::logic_error(
        "FilterCache validation: stale publication did not acquire claim");
  }

  // Equal-core publication is permitted by the cache API. Replacing its active
  // identity retires the old slot without the core-generation backstop, so the
  // slot and active-snapshot checks are independently observable here.
  std::array replacement{FilterCache::SegmentIdentity{2, 256}};
  cache.onReaderPublished(7, replacement);
  DocSetBuilder builder(256);
  builder.add(3);
  use->publishRaw(0, claim, builder.build(), 1);
  cache.validateForTest();
  if (cache.bytesUsed() != 0) {
    throw std::logic_error(
        "FilterCache validation: stale publication became resident");
  }
}

void validateConcurrentSweepPurgeAccounting() {
#ifdef LUXIR_ASAN
  constexpr int entries = 8192;
#else
  constexpr int entries = 32768;
#endif
  FilterCacheConfig config{
      .maxBytes = 64 * 1024 * 1024,
      .maxEntryBytes = 1024 * 1024,
      .minSegmentDocs = 0,
      .admissionHistorySize = entries,
      .admissionThreshold = 1,
      .maxMetadataEntries = entries * 2,
      .maxMetadataBytes = 64 * 1024 * 1024};
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 256}};
  cache.onReaderPublished(1, segments);
  for (int i = 0; i < entries; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(FilterKey("sweep-purge-" + std::to_string(i)));
    DocSetBuilder builder(256);
    builder.add(i % 255);
    use->offerRaw(0, builder.build(), 1);
  }

  std::atomic<bool> sweepStarted{false};
  std::thread sweeper([&] {
    sweepStarted.store(true, std::memory_order_release);
    cache.sweep();
  });
  while (!sweepStarted.load(std::memory_order_acquire)) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::array replacement{FilterCache::SegmentIdentity{2, 256}};
  cache.onReaderPublished(2, replacement);
  sweeper.join();
  cache.validateForTest();
  auto counters = cache.counters();
  if (counters.residentBytes != 0
      || counters.evictions + counters.purges != entries) {
    throw std::logic_error(
        "FilterCache validation: sweep/purge detach was not exactly once");
  }
}

} // namespace

class FilterCacheConcurrencyTest : public LuxirTest {};

TEST_F(FilterCacheConcurrencyTest, updateMergePurgeEvictFuzz) {
  SCOPED_TRACE("gtest seed=" + std::to_string(global_random_seed)
               + " test seed=" + std::to_string(rng_seed));
  ASSERT_NO_THROW(validateConcurrentByproductPublication());
  ASSERT_NO_THROW(validateStalePublicationRejection());
  ASSERT_NO_THROW(validateConcurrentSweepPurgeAccounting());

  constexpr std::string_view collection = "filter_cache_concurrency";
  const int writerThreads = 3;
  const int queryThreads = 4;
  const int phases = 3;
  const int percentDeletes = 35;
  const int percentCommits = 70;
#ifdef LUXIR_ASAN
  const int batchesPerWriter = 18;
#else
  const int batchesPerWriter = 40;
#endif

  LuxirConfig nodeConfig;
  nodeConfig.queryCacheBytes = 0;
  LuxirNode node(nodeConfig);
  CollectionHelper helper(node, collection);
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(3);
  writer->perInverterMaxDocs = 4;
  writer->perInverterRamBytes = 16 * 1024;

  FilterCacheConfig cacheConfig{
      .maxBytes = 768,
      .maxEntryBytes = 512,
      .minSegmentDocs = 0,
      .admissionHistorySize = 64,
      .admissionThreshold = 2,
      .maxMetadataEntries = 32,
      .maxMetadataBytes = 16 * 1024};
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  writer->snapshots.readers.filterCache = cache;

  // Seed the bounded universe in small committed batches. The low merge factor
  // turns these into a mix of live tiers before concurrent mutation begins.
  for (int32_t base = 0; base < DOC_UNIVERSE; base += 8) {
    std::vector<Doc> docs;
    for (int32_t id = base; id < std::min(base + 8, DOC_UNIVERSE); id++) {
      docs.push_back(docFor(id));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT,
                                /*overwrite=*/true).success);
  }
  ASSERT_TRUE(commitAndDrain(helper).success);
  ASSERT_NO_THROW(cache->validateForTest());

  // Two sightings admit; the third search makes each surviving entry hot.
  for (int repeat = 0; repeat < 3; repeat++) {
    for (size_t i = 0; i < (size_t)FilterKind::COUNT; i++) {
      auto result = runSearch(node.getSearchEngine(), collection,
                              (FilterKind)i);
      ASSERT_TRUE(result.valid) << result.error;
    }
  }
  ASSERT_NO_THROW(cache->validateForTest());

  uint64_t requestSeed = rng();
  for (int phase = 0; phase < phases; phase++) {
    FailureState failures;
    std::latch start(1);
    const int searchesThisPhase = writerThreads * batchesPerWriter;
    std::counting_semaphore<> searchReady(0);
    std::vector<std::thread> threads;

    for (int tid = 0; tid < queryThreads; tid++) {
      threads.emplace_back([&, tid] {
        start.wait();
        for (int task = tid; task < searchesThisPhase; task += queryThreads) {
          searchReady.acquire();
          if (failures.failed.load(std::memory_order_relaxed)) continue;
          auto kind = (FilterKind)((task + phase) % (int)FilterKind::COUNT);
          auto result = runSearch(node.getSearchEngine(), collection, kind);
          if (!result.valid) failures.record(std::move(result.error));
        }
      });
    }

    for (int tid = 0; tid < writerThreads; tid++) {
      threads.emplace_back([&, tid] {
        Rng random(requestSeed + (uint64_t)(phase * writerThreads + tid));
        start.wait();
        for (int batch = 0; batch < batchesPerWriter; batch++) {
          CollectionHelper::UpdateBuilder update;
          int operations = random.rint(2, 5);
          for (int op = 0; op < operations; op++) {
            int32_t id = random.rint(DOC_UNIVERSE);
            if (random.rint(100) < percentDeletes) {
              update.remove(std::to_string(id));
            } else {
              update.add(docFor(id));
            }
          }
          update.overwrite(true);
          if (random.rint(100) < percentCommits) update.commit();
          auto result = helper.submit(update);
          if (!result.success) {
            failures.record("update batch failed: " + result.error_message);
          }
          // One query task is released only after each update batch completes.
          searchReady.release();
        }
      });
    }

    start.count_down();
    for (auto& thread : threads) thread.join();
    ASSERT_FALSE(failures.failed.load(std::memory_order_relaxed))
        << failures.firstFailure;
    ASSERT_TRUE(commitAndDrain(helper).success);
    ASSERT_NO_THROW(cache->validateForTest()) << "phase=" << phase;
  }

  // With writers stopped, derive truth from one unfiltered reader snapshot and
  // require exact parity from repeatedly warmed filtered requests.
  ASSERT_TRUE(commitAndDrain(helper).success);
  auto all = runSearch(node.getSearchEngine(), collection, std::nullopt);
  ASSERT_TRUE(all.valid) << all.error;
  ASSERT_EQ(std::set<int32_t>(all.ids.begin(), all.ids.end()).size(),
            all.ids.size()) << "ids=" << ::testing::PrintToString(all.ids);
  for (size_t i = 0; i < (size_t)FilterKind::COUNT; i++) {
    auto kind = (FilterKind)i;
    std::vector<int32_t> expected;
    for (int32_t id : all.ids) {
      if (matchesFilter(kind, id)) expected.push_back(id);
    }
    SearchResult actual;
    for (int repeat = 0; repeat < 3; repeat++) {
      actual = runSearch(node.getSearchEngine(), collection, kind);
      ASSERT_TRUE(actual.valid) << actual.error;
    }
    EXPECT_EQ(expected, actual.ids)
        << "quiesced exact parity oracle: filter=" << filterName(kind);
  }
  ASSERT_NO_THROW(cache->validateForTest());

  auto counters = cache->counters();
  EXPECT_GT(counters.hits, 0u);
  EXPECT_GT(counters.builds, 0u);
  EXPECT_GT(counters.evictions, 0u);
  EXPECT_GT(counters.purges, 0u);
  EXPECT_LE(counters.evictions + counters.purges,
            counters.builds + counters.byproductInserts);

  cache->clear();
  ASSERT_NO_THROW(cache->validateForTest());
  counters = cache->counters();
  EXPECT_EQ(0u, counters.residentBytes);
  EXPECT_EQ(0u, counters.metadataBytes);
  EXPECT_EQ(0u, cache->entryCountForTest());
}

namespace {

struct PolicyTrace {
  FilterCache cache;
  std::vector<FilterCache::SegmentIdentity> segments{{1, 4096}};
  uint64_t generation = 1;

  PolicyTrace(size_t window, size_t high,
              uint32_t threshold = 1, uint64_t workEpochs = 0)
      : cache([&] {
          auto config = testConfig();
          config.recentWorkEpochs = workEpochs ? workEpochs : window;
          config.admissionHistorySize = window;
          config.admissionThreshold = threshold;
          config.maxBytes = high;

          config.maxMetadataEntries = 4096;
          config.maxMetadataBytes = 64 << 20;
          return config;
        }()) {
    cache.onReaderPublished(generation, segments);
  }

  bool serve(const std::string& key, uint64_t cost = 100, bool large = true) {
    FilterCache::UseRegistry request(cache, generation, segments);
    auto* use = request.get(FilterKey(key));
    bool allHit = true;
    for (size_t i = 0; i < segments.size(); i++) {
      auto probe = use->probe(i);
      if (probe.kind() == FilterCache::Probe::Kind::HIT) {
        EXPECT_TRUE(probe.docSet()->get(0));
      } else {
        allHit = false;
        if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
          auto raw = large ? bitDocs(segments[i].maxDoc, 0)
                           : docs(segments[i].maxDoc, {0});
          use->publishRaw(i, probe, std::move(raw), cost);
        }
      }
    }
    return allHit;
  }

  bool serveReader(IndexReader& reader, const std::string& key) {
    auto domains = canonicalDomains(reader);
    FilterCache::UseRegistry request(cache, reader);
    auto* use = request.get(FilterKey(key), FilterKeyScope::READER_STABLE);
    auto probe = use->probeReaderStable(reader, domains);
    if (probe.kind() == FilterCache::ReaderProbe::Kind::HIT) return true;
    if (probe.kind() == FilterCache::ReaderProbe::Kind::BUILD) {
      use->publishReaderStable(probe, oneDocPerSegment(reader), 100);
    }
    return false;
  }

  FilterCache::CounterValues report(const char* scenario, int convergence = 0) {
    auto c = cache.counters();
    std::printf("trace %-16s epochs=%llu hits=%llu misses=%llu "
                "build_ns=%llu unrepaid=%llu deferred=%llu converge=%d\n",
                scenario,
                (unsigned long long)cache.configuration().recentWorkEpochs,
                (unsigned long long)c.hits, (unsigned long long)c.misses,
                (unsigned long long)c.buildNanos,
                (unsigned long long)(c.unrepaidEvictions),
                (unsigned long long)(c.populationDeferred),
                convergence);
    cache.validateForTest();
    return c;
  }
};

} // namespace

TEST(FilterCacheTest, zipf) {
  // A Zipf-like head with heterogeneous sizes/costs and rolling two-use keys.
  size_t charge = bitDocs(4096, 0)->ramBytesUsed();
  {
    PolicyTrace trace(256, charge * 6, 2);
    for (int round = 0; round < 32; round++) {
      for (int i = 0; i < 8; i++) {
        for (int n = 0; n < 32 / (i + 1); n++) {
          trace.serve("head-" + std::to_string(i), 100 * (1 + i % 4), i % 2 == 0);
        }
      }
      for (int i = 0; i < 4; i++) {
        for (int n = 0; n < 2; n++) {
          trace.serve("tail-" + std::to_string(4 * round + i), 150, i % 2 == 0);
        }
      }
    }
    auto c = trace.report("zipf");
    EXPECT_GE(c.hits, 2600u);
    EXPECT_LE(c.buildNanos, 30000u);
    EXPECT_GT(c.evictions, 0u);
  }
}

TEST(FilterCacheTest, lowDensityHead) {
  // The head costs less per effective byte, but earns its space by reuse.
  size_t charge = bitDocs(4096, 0)->ramBytesUsed();
  {
    PolicyTrace trace(128, charge * 4, 2);
    int headHits = 0;
    for (int round = 0; round < 64; round++) {
      for (int n = 0; n < 8; n++) headHits += trace.serve("cheap-hot-head", 100);
      for (int i = 0; i < 4; i++) {
        for (int n = 0; n < 2; n++) {
          trace.serve("dense-tail-" + std::to_string(4 * round + i), 120, false);
        }
      }
    }
    auto c = trace.report("low-density-head");
    EXPECT_GE(headHits, 500);
    EXPECT_LE(c.buildNanos, 32000u);
  }
}

TEST(FilterCacheTest, repeatedNewcomer) {
  // The budget holds six values; each newcomer build forces one eviction.
  size_t charge = bitDocs(4096, 0)->ramBytesUsed();
  {
    PolicyTrace trace(4096, charge * 6 + charge / 2, 1, 16);
    for (int round = 0; round < 16; round++) {
      for (int i = 0; i < 6; i++) trace.serve("old-" + std::to_string(i));
    }
    trace.cache.resetCounters();
    int requests = 0;
    while (++requests <= 64 && !trace.serve("newcomer")) {}
    EXPECT_LE(requests, 64);
    trace.report("newcomer", requests);
  }
}

TEST(FilterCacheTest, workingSetShift) {
  size_t charge = bitDocs(4096, 0)->ramBytesUsed();
  {
    PolicyTrace trace(4096, charge * 20, 1, 64);
    for (int round = 0; round < 21; round++) {
      for (int i = 0; i < 16; i++) trace.serve("old-" + std::to_string(i));
    }
    trace.cache.resetCounters();
    int convergence = 0;
    for (int round = 1; round <= 16; round++) {
      int hits = 0;
      for (int i = 0; i < 16; i++) hits += trace.serve("new-" + std::to_string(i));
      if (hits == 16 && convergence == 0) convergence = round;
    }
    EXPECT_GT(convergence, 0);

    trace.report("working-set-shift", convergence);
  }
}

TEST(FilterCacheTest, segmentTurnover) {
  size_t charge = bitDocs(4096, 0)->ramBytesUsed();
  {
    PolicyTrace trace(128, charge * 6 + charge / 2);
    trace.segments.push_back({2, 4096});
    ASSERT_TRUE(trace.cache.onReaderPublished(++trace.generation, trace.segments));
    for (int round = 0; round < 16; round++) {
      for (int i = 0; i < 3; i++) trace.serve("hot-" + std::to_string(i));
    }
    trace.cache.resetCounters();
    trace.segments[1].segId = 3;
    ASSERT_TRUE(trace.cache.onReaderPublished(++trace.generation, trace.segments));
    for (int i = 0; i < 3; i++) trace.serve("filler-" + std::to_string(i));
    int convergence = 0;
    for (int round = 1; round <= 8; round++) {
      int hits = 0;
      for (int i = 0; i < 3; i++) hits += trace.serve("hot-" + std::to_string(i));
      if (hits == 3 && convergence == 0) convergence = round;
    }
    EXPECT_GT(convergence, 0);
    for (int i = 0; i < 3; i++) EXPECT_TRUE(trace.serve("hot-" + std::to_string(i)));
    trace.report("segment-turnover", convergence);
  }
}

TEST(FilterCacheTest, readerRefresh) {
  {
    LuxirConfig config;
    config.queryCacheBytes = 0;
    LuxirNode node(config);
    test::CollectionHelper helper(node, "trace_reader_refresh");
    ASSERT_TRUE(helper.indexAll(std::array{
        test::flatdoc("id", "1", "body_w", "body"),
        test::flatdoc("id", "2", "body_w", "body"),
        test::flatdoc("id", "3", "body_w", "body")}, UpdateMessage::COMMIT).success);
    auto writer = helper.getIndexWriter();
    auto reader = writer->snapshots.readers.getReader();
    PolicyTrace trace(128, 1024 * 1024);
    ASSERT_TRUE(trace.cache.onReaderPublished(*reader));
    trace.segments = readerIdentitiesForTest(*reader);
    trace.generation = reader->coreGen();
    for (int round = 0; round < 16; round++) {
      for (int i = 0; i < 3; i++) trace.serveReader(*reader, "reader-" + std::to_string(i));
      trace.serve("core-incumbent");
    }
    size_t warmBytes = trace.cache.bytesUsed();
    trace.cache.resize(warmBytes + 1);
    ASSERT_EQ(warmBytes, trace.cache.bytesUsed());
    ASSERT_EQ(4u, trace.cache.dump(10, nullptr).size());
    trace.cache.resetCounters();
    std::vector<std::string> deletes{"3"};
    helper.deleteByIds(deletes, UpdateMessage::COMMIT);
    auto nextReader = writer->snapshots.readers.getReader();
    ASSERT_TRUE(trace.cache.onReaderPublished(*nextReader));
    trace.serve("core-pressure");
    int convergence = 0;
    for (int round = 1; round <= 8; round++) {
      int hits = 0;
      for (int i = 0; i < 3; i++) hits += trace.serveReader(*nextReader, "reader-" + std::to_string(i));
      if (hits == 3 && convergence == 0) convergence = round;
    }
    EXPECT_GT(convergence, 0);
    EXPECT_GT(trace.cache.counters().readerStableRefreshes, 0u);
    trace.report("reader-refresh", convergence);
  }
}

TEST(FilterCacheTest, scalarFactsUseOnlyTrackedSlotMetadata) {
  FilterCache cache(testConfig());
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("scalar-fact");
  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    auto before = cache.counters();
    EXPECT_FALSE(use->recordCardinality(0, 7));
    EXPECT_FALSE(use->cardinality(0));
    EXPECT_EQ(before, cache.counters());
    EXPECT_EQ(0u, cache.entryCountForTest());
  }
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  auto before = cache.counters();
  EXPECT_TRUE(use->recordCardinality(0, 7));
  EXPECT_FALSE(use->recordCardinality(0, 7));
  EXPECT_FALSE(use->recordCardinality(0, -1));
  EXPECT_FALSE(use->recordCardinality(1, 0));
  EXPECT_EQ(7, use->cardinality(0));
  EXPECT_EQ(before, cache.counters());
  const auto& ledger = ledgerFor(cache, key);
  EXPECT_EQ(0, ledger.owed);
  EXPECT_EQ(0, ledger.debt);
  EXPECT_EQ(0, ledger.productionNanos);
  EXPECT_EQ(0u, ledger.referenceBytes);
  {
    auto ordinary = use->probe(0, FilterCache::Work::pending());
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, ordinary.kind());
    EXPECT_FALSE(use->recordCardinality(0, 7));
  }
  request.addSegmentNanos(0, 1000);
  request.settle(true);
  EXPECT_EQ(1u, ledgerFor(cache, key).absentCount);
  EXPECT_EQ(1000, ledgerFor(cache, key).owed);
  auto claim = use->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::BUILD, claim.kind());
  use->publishRaw(0, claim, docs(100, {0, 1, 2, 3, 4, 5, 6}), 1000);
  evictValues(cache);
  EXPECT_EQ(7, use->cardinality(0));
  std::array replacement{FilterCache::SegmentIdentity{2, 100}};
  ASSERT_TRUE(cache.onReaderPublished(2, replacement));
  EXPECT_FALSE(use->cardinality(0));
  EXPECT_FALSE(use->recordCardinality(0, 7));
}

TEST(FilterCacheTest, completeCountPreflightIsInertAndPinsFactsAcrossRetirement) {
  auto config = testConfig();
  config.admissionThreshold = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}, FilterCache::SegmentIdentity{2, 100}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey key("complete-counts");
  FilterCache::UseRegistry learning(cache, 1, segments), accepted(cache, 1, segments), racing(cache, 1, segments);
  auto* source = learning.get(key);
  ASSERT_TRUE(source->recordCardinality(0, 2));
  auto before = cache.counters();
  EXPECT_FALSE(accepted.lookupExisting(key, FilterKeyScope::SEGMENT_STABLE, true));
  EXPECT_EQ(before, cache.counters());
  ASSERT_TRUE(source->recordCardinality(1, 0));
  auto candidate = accepted.lookupExisting(key, FilterKeyScope::SEGMENT_STABLE, true);
  ASSERT_TRUE(candidate);
  auto* use = accepted.acceptExisting(std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, use);
  EXPECT_EQ(2, use->acceptedCardinality(0));
  EXPECT_EQ(0, use->acceptedCardinality(1));
  EXPECT_EQ(before, cache.counters());
  auto stale = racing.lookupExisting(key, FilterKeyScope::SEGMENT_STABLE, true);
  ASSERT_TRUE(stale);
  cache.clear();
  EXPECT_EQ(nullptr, racing.acceptExisting(std::move(*stale), FilterCache::AdmissionLane::WHOLE));
  EXPECT_EQ(2, use->acceptedCardinality(0));
  EXPECT_EQ(0, use->acceptedCardinality(1));
  EXPECT_FALSE(use->cardinality(0));
}
