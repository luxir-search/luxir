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

#include "solux/index/IndexWriter.h"
#include "solux/index/Inverter.h"
#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/ConstantScoreQuery.h"
#include "solux/query/ExistsQuery.h"
#include "solux/query/ForcePrepareQuery.h"
#include "solux/query/FuzzyQuery.h"
#include "solux/query/GeoBoxQuery.h"
#include "solux/query/GeoDistanceQuery.h"
#include "solux/query/KnnQuery.h"
#include "solux/query/MatchNoDocsQuery.h"
#include "solux/query/NumericRangeQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/PrefixQuery.h"
#include "solux/query/QueryPrep.h"
#include "solux/query/TermQuery.h"
#include "solux/query/TermRangeQuery.h"
#include "solux/search/FilterCache.h"
#include "solux/search/SearchOverrides.h"
#include "solux/server/SoluxNode.h"
#include "solux/store/Directory.h"
#include "solux/util/DateTime.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;

namespace {

FilterCacheConfig testConfig() {
  return {
      .maxBytes = 1024 * 1024,
      .lowWatermarkBytes = 900 * 1024,
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
  explicit UncacheableQuery(Query& child) : child(child) {}

  ScoreProfile scoreProfile() const override {
    return child.scoreProfile();
  }

  FilterKeyScope appendFilterKey(
      FilterKeyBuilder& out, const FilterKeyContext& ctx) const override {
    unused(out, ctx);
    return FilterKeyScope::UNCACHEABLE;
  }

  Query::Weight* createWeight(
      Query::Context& context, int32_t flags,
      float multiplier = 1.0f) override {
    return child.createWeight(context, flags, multiplier);
  }
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

CachedSearchResult runCachedSearch(SoluxNode& node, std::string_view collection,
                                   std::string_view filter) {
  auto request = solux::test::localReq(node.getSearchEngine());
  request->collection(collection)
      .topDocs("q")
      .matchQuery("body_w", "body")
      .matchFilter("selection", "filter_w", filter)
      .fields({"id"})
      .getNumber()
      .limit(-1);
  request->execute();
  EXPECT_TRUE(request->ok()) << request->toString();
  CachedSearchResult result{
      .count = request->getMatchCount("q"), .ids = {}};
  for (const auto& doc : request->getDocs("q")) {
    auto* id = solux::test::find(doc, "id");
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

void addFilter(solux::test::OpCursor& cursor, const api::Query& query);

CachedSearchResult runKnnFilter(
    SoluxNode& node, std::string_view collection,
    std::span<const float> queryVector, int32_t k, bool exact = true,
    int32_t nprobe = 0, int32_t refineCandidates = 0,
    float minScanFraction = 0.0f) {
  auto request = solux::test::localReq(node.getSearchEngine());
  auto& cursor = request->collection(collection)
                     .topDocs("q")
                     .allQuery()
                     .fields({"id"})
                     .getNumber()
                     .limit(-1);
  addFilter(cursor, solux::test::qb::knn(
      cursor.mr(), "embedding_v", queryVector, k, nprobe, exact,
      refineCandidates, minScanFraction));
  request->execute();
  EXPECT_TRUE(request->ok()) << request->toString();
  CachedSearchResult result{
      .count = request->getMatchCount("q"), .ids = {}};
  for (const auto& doc : request->getDocs("q")) {
    auto* id = solux::test::find(doc, "id");
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
    result.push_back(segment.liveDocs() == nullptr
        ? nullptr : &segment.liveDocs()->docset());
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

void addFilter(solux::test::OpCursor& cursor, const api::Query& query) {
  auto& top = std::get<api::TopDocs>(cursor.rawOp().kind);
  auto* filter = api::build::allocArray(top.filter, 1, cursor.mr());
  filter[0].name = "selection";
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

TEST(FilterCacheTest, entryCapRejectsAndMatchAllDoesNotPopulate) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = 1;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 100}};
  cache.onReaderPublished(1, segments);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* tooLarge = request.get(FilterKey("large"));
  auto tooLargeProbe = tooLarge->probe(0);
  tooLarge->publishRaw(0, tooLargeProbe, docs(100, {1}), 1);
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
}

TEST(FilterCacheTest, evictsLowerBenefitDensityAndPinsRetiredValue) {
  auto sample = bitDocs(1024, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;
  config.lowWatermarkBytes = charge;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey first("cheap");
  FilterKey second("expensive");
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* firstUse = request.get(first);
  auto firstProbe = firstUse->probe(0);
  auto pinned = firstUse->publishRaw(
      0, firstProbe, std::move(sample), 10);
  auto* secondUse = request.get(second);
  auto secondProbe = secondUse->probe(0);
  secondUse->publishRaw(0, secondProbe, bitDocs(1024, 2), 10'000);

  EXPECT_EQ(charge, cache.bytesUsed());
  EXPECT_EQ(1u, cache.counters().evictions);
  EXPECT_TRUE(pinned->docSet()->get(1));
  FilterCache::UseRegistry verify(cache, 1, segments);
  EXPECT_NE(FilterCache::Probe::Kind::HIT, verify.get(first)->probe(0).kind());
  EXPECT_EQ(FilterCache::Probe::Kind::HIT, verify.get(second)->probe(0).kind());
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, densityIncludesEffectiveResidencyForTinyPayloads) {
  auto tiny = docs(4096, {1});
  auto large = bitDocs(4096, 2);
  size_t tinyCharge = tiny->ramBytesUsed();
  size_t largeCharge = large->ramBytesUsed();
  constexpr uint32_t TINY_COST = 100;
  constexpr uint32_t LARGE_COST = 1000;
  // Payload-only density ranks tiny first, which is the pathology this test
  // excludes once fixed/value and per-entry residency are included.
  ASSERT_GT((double) TINY_COST / (double) tinyCharge,
            (double) LARGE_COST / (double) largeCharge);

  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = largeCharge + 1;
  config.maxBytes = tinyCharge + largeCharge - 1;
  config.lowWatermarkBytes = largeCharge;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey tinyKey("tiny-density");
  FilterKey largeKey("wide-density");
  ASSERT_EQ(tinyKey.bytes().capacity(), largeKey.bytes().capacity());

  FilterCache::UseRegistry request(cache, 1, segments);
  auto* tinyUse = request.get(tinyKey);
  auto tinyProbe = tinyUse->probe(0);
  tinyUse->publishRaw(0, tinyProbe, std::move(tiny), TINY_COST);
  auto* largeUse = request.get(largeKey);
  auto largeProbe = largeUse->probe(0);
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

TEST(FilterCacheTest, inflationClockEventuallyAgesOutColdExpensiveValue) {
  auto sample = bitDocs(1024, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;
  config.lowWatermarkBytes = charge;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 1024}};
  cache.onReaderPublished(1, segments);
  FilterKey expensive("cold-expensive");

  {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(expensive);
    auto probe = use->probe(0);
    use->publishRaw(0, probe, std::move(sample), 100);
  }

  for (int i = 0; i < 12; i++) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(
        FilterKey("cheap-" + std::to_string(i)));
    auto probe = use->probe(0);
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
  config.maxMetadataBytes = 1024;
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
  config.lowWatermarkBytes = 0;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("concurrent");
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
    for (int i = 0; i < 500; i++) cache.sweep();
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
  cache.clear();
  EXPECT_EQ(0u, cache.bytesUsed());
  EXPECT_GT(cache.counters().builds + cache.counters().byproductInserts, 0u);
}

TEST(FilterCacheTest, concurrentPublicationsFinishBelowLowWatermark) {
  auto sample = bitDocs(4096, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;
  config.lowWatermarkBytes = charge;
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
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "cache me");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.getIndexReader();
  auto cache = writer.getFilterCache();
  TermQuery query("text_w", "cache");

  {
    MemPool contextPool;
    Query::Context context(contextPool, *reader);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query);
    MemPool execPool;
    auto* supplier = QueryPrep::filterSupplier(
        execPool, *weight, nullptr, use, *reader, reader->segments()[0]);
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
        execPool, *weight, nullptr, use, *reader, reader->segments()[0]);
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
        execPool, *weight, nullptr, use, *reader, reader->segments()[0]);
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
  IndexWriter writer(dir, {}, nullptr, config);
  for (int32_t doc = 0; doc < 1024; doc++) {
    addTermDoc(writer, doc == 7 ? "selected body" : "body");
  }
  writer.commit();
  auto reader = writer.getIndexReader();
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
      firstPool, *weight, nullptr, use, *reader, reader->segments()[0],
      QueryPrep::FilterSupplierMode::SPARSE_BATCH, 64);
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(first));
  EXPECT_EQ(0, SkipStats::ownedFilterMaterializations);
  EXPECT_EQ(0, SkipStats::ownedFilterServes);

  auto before = reader->filterCache()->counters();
  MemPool secondPool;
  auto* second = QueryPrep::filterSupplier(
      secondPool, *weight, nullptr, use, *reader, reader->segments()[0],
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
      disabledExecPool, *disabledWeight, nullptr, disabledUse, *reader,
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
  IndexWriter writer(dir, {}, nullptr, config);
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
  auto reader = writer.getIndexReader();
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
    auto* bulk = pool.make<BooleanQuery::DocSetDisjunctionBulkScorer>(
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
  IndexWriter writer(dir);
  for (int32_t doc = 0; doc < 8; doc++) {
    addIdTermDoc(writer, std::to_string(doc), "selected");
  }
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("1", 1);
  deletes.deleteId("3", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = writer.getIndexReader(0);
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

TEST(FilterCacheTest, uncacheableFilterStaysPostingsBacked) {
  FilterCacheConfig config = testConfig();
  config.maxBytes = 0;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "selected");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.getIndexReader();

  TermQuery term("text_w", "selected");
  UncacheableQuery query(term);
  MemPool contextPool;
  Query::Context context(contextPool, *reader);
  EXPECT_EQ(nullptr, context.getFilterUse(query));
  auto* weight = query.createWeight(context, 0);
  MemPool execPool;
  auto* supplier = QueryPrep::filterSupplier(
      execPool, *weight, nullptr, context.getFilterUse(query), *reader,
      reader->segments()[0],
      QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE);
  ASSERT_NE(nullptr, supplier);
  EXPECT_EQ(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(supplier));
}

TEST(FilterCacheTest, ownedSupplierPredicateIsModeSpecific) {
  FilterCacheConfig config = testConfig();
  config.maxBytes = 0;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  constexpr int32_t maxDoc = 1024;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    std::string terms = "body";
    if (doc < 4) terms += " sparse";
    if (doc < 24) terms += " middle";
    if (doc < 40) terms += " over";
    addTermDoc(writer, terms);
  }
  writer.commit();
  auto reader = writer.getIndexReader();
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
        execPool, *weight, nullptr, use, *reader, reader->segments()[0],
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
  IndexWriter writer(dir, {}, nullptr, config);
  constexpr int32_t maxDoc = 1024;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    addTermDoc(writer, doc == 0 ? "alpha beta selected" : "alpha beta");
  }
  writer.commit();
  auto reader = writer.getIndexReader();
  auto cache = writer.getFilterCache();

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
     exactScoredSparseFilterPostingsFeedMatchesCache) {
  SoluxConfig cachedConfig;
  cachedConfig.filterCacheBytes = 4 * 1024 * 1024;
  SoluxConfig uncachedConfig;
  uncachedConfig.filterCacheBytes = 0;
  SoluxNode cachedNode(cachedConfig);
  SoluxNode uncachedNode(uncachedConfig);
  constexpr std::string_view collection =
      "exact_scored_sparse_filter_postings";
  solux::test::CollectionHelper cached(cachedNode, collection);
  solux::test::CollectionHelper uncached(uncachedNode, collection);

  constexpr int32_t maxDoc = 12 * DocsEnumMeta::L1_DOCS + 257;
  std::vector<solux::test::Doc> docs;
  docs.reserve((size_t) maxDoc);
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    std::string body = "alpha";
    for (int32_t repeat = 0; repeat < doc % 5; repeat++) {
      body += " alpha";
    }
    docs.push_back(solux::test::flatdoc(
        "id", std::to_string(doc), "body_w", body,
        "filter_w", doc % 40 == 0 ? "selected" : "other"));
  }
  ASSERT_TRUE(cached.indexAll(docs, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(uncached.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Result {
    int64_t count = 0;
    std::vector<std::string> ids;
    std::vector<float> scores;
    int64_t batchEngagements = 0;
    int64_t postingsFeedEngagements = 0;
  };
  auto run = [&](SoluxNode& node, bool disablePostingsFeed) {
    FilteredConjunctionPostingsFeedGuard feedGuard(
        disablePostingsFeed);
    OwnedFilterStatsGuard statsGuard;
    auto request = solux::test::localReq(node.getSearchEngine());
    request->collection(collection)
        .topDocs("q")
        .matchQuery("body_w", "alpha")
        .matchFilter("selection", "filter_w", "selected")
        .fields({"id"})
        .withStats()
        .limit(100);
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
    result.postingsFeedEngagements =
        SkipStats::filteredConjBatchPostingsFeedEngagements;
    return result;
  };

  run(cachedNode, false);
  run(cachedNode, false);
  Result cachedResult = run(cachedNode, false);
  Result postingsResult = run(uncachedNode, false);
  Result disabledResult = run(uncachedNode, true);

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
}

enum class ExactFilteredConjShape {
  THREE_TERMS,
  RARE_TERM,
  PHRASE,
  TWO_FILTERS,
};

struct ExactFilteredConjResult {
  int64_t count = 0;
  std::vector<std::string> ids;
  std::vector<float> scores;
  int64_t batchEngagements = 0;
  int64_t multiTermEngagements = 0;
  int64_t postingsFeedEngagements = 0;
};

bool indexExactFilteredConjDocs(SoluxNode& node,
                                std::string_view collection) {
  constexpr int32_t maxDoc = 2000;
  solux::test::CollectionHelper helper(node, collection);
  std::vector<solux::test::Doc> docs;
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
    docs.push_back(solux::test::flatdoc(
        "id", std::to_string(doc), "body_w", body,
        "filter_w", doc % 40 == 0 ? "selected" : "other",
        "second_filter_w", doc % 20 == 0 ? "wide" : "other"));
  }
  return helper.indexAll(docs, UpdateMessage::COMMIT).success;
}

ExactFilteredConjResult runExactFilteredConj(
    SoluxNode& node, std::string_view collection,
    ExactFilteredConjShape shape, bool disableMultiTerm,
    bool disableBatch = false) {
  FilteredConjMultiTermGuard multiTermGuard(disableMultiTerm);
  FilteredConjunctionBatchGuard batchGuard(disableBatch);
  OwnedFilterStatsGuard statsGuard;
  auto request = solux::test::localReq(node.getSearchEngine());
  request->collection(collection);
  auto& cursor = request->topDocs("q");
  cursor.getNumber().withStats().fields({"id"}).limit(100);
  switch (shape) {
    case ExactFilteredConjShape::THREE_TERMS:
      cursor.rawQuery() = solux::test::qb::boolean(
          cursor.mr(),
          {solux::test::qb::match(cursor.mr(), "body_w", "alpha"),
           solux::test::qb::match(cursor.mr(), "body_w", "beta"),
           solux::test::qb::match(cursor.mr(), "body_w", "gamma")});
      cursor.matchFilter("selection", "filter_w", "selected");
      break;
    case ExactFilteredConjShape::RARE_TERM:
      cursor.rawQuery() = solux::test::qb::boolean(
          cursor.mr(),
          {solux::test::qb::match(cursor.mr(), "body_w", "alpha"),
           solux::test::qb::match(cursor.mr(), "body_w", "rare")});
      cursor.matchFilter("selection", "filter_w", "selected");
      break;
    case ExactFilteredConjShape::PHRASE:
      cursor.rawQuery() = solux::test::qb::phraseWords(
          cursor.mr(), "body_w", {"quick", "fox"});
      cursor.matchFilter("selection", "filter_w", "selected");
      break;
    case ExactFilteredConjShape::TWO_FILTERS:
      cursor.rawQuery() = solux::test::qb::boolean(
          cursor.mr(),
          {solux::test::qb::match(cursor.mr(), "body_w", "alpha"),
           solux::test::qb::match(cursor.mr(), "body_w", "beta")});
      cursor.matchFilter("selection", "filter_w", "selected");
      cursor.matchFilter("wide", "second_filter_w", "wide");
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
  SoluxConfig cachedConfig;
  cachedConfig.filterCacheBytes = 4 * 1024 * 1024;
  SoluxConfig uncachedConfig;
  uncachedConfig.filterCacheBytes = 0;
  SoluxNode cachedNode(cachedConfig);
  SoluxNode uncachedNode(uncachedConfig);
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
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
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

TEST(FilterCacheIntegrationTest, filteredConjMultiTermRequiresFilterLead) {
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
  constexpr std::string_view collection = "filtered_conj_filter_lead";
  ASSERT_TRUE(indexExactFilteredConjDocs(node, collection));

  ExactFilteredConjResult enabled = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::RARE_TERM, false);
  ExactFilteredConjResult baseline = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::RARE_TERM, true);

  EXPECT_EQ(10, enabled.count);
  expectSameExactFilteredConj(baseline, enabled);
  EXPECT_EQ(0, enabled.batchEngagements);
  EXPECT_EQ(0, enabled.multiTermEngagements);
}

TEST(FilterCacheIntegrationTest, filteredConjPhraseTailDeclines) {
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
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

TEST(FilterCacheIntegrationTest, filteredConjTwoTermFiltersPreserveScores) {
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
  constexpr std::string_view collection = "filtered_conj_two_filters";
  ASSERT_TRUE(indexExactFilteredConjDocs(node, collection));

  ExactFilteredConjResult enabled = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::TWO_FILTERS, false);
  ExactFilteredConjResult baseline = runExactFilteredConj(
      node, collection, ExactFilteredConjShape::TWO_FILTERS, true);

  EXPECT_EQ(50, enabled.count);
  expectSameExactFilteredConj(baseline, enabled);
  EXPECT_GT(enabled.multiTermEngagements, 0);
  EXPECT_GT(enabled.postingsFeedEngagements, 0);
  EXPECT_EQ(0, baseline.multiTermEngagements);
}

TEST(FilterCacheTest, effectiveMaterializationOffersRawByproduct) {
  FilterCacheConfig config = testConfig();
  config.minSegmentDocs = 0;
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "cache me");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.getIndexReader();
  auto cache = writer.getFilterCache();
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
      *racerWeight, nullptr, racerUse, *reader, reader->segments()[0], nullptr);
  ASSERT_NE(nullptr, effective.get());
  EXPECT_EQ(1, effective.get()->card());
  EXPECT_EQ(1u, cache->counters().byproductInserts);
}

TEST(FilterCacheTest, pruningBypassDoesNotOfferByproduct) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "cache me");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.getIndexReader();
  auto cache = writer.getFilterCache();
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
      *racerWeight, nullptr, racerUse, *reader, reader->segments()[0], nullptr);

  ASSERT_NE(nullptr, effective.get());
  EXPECT_EQ(1, effective.get()->card());
  EXPECT_EQ(0u, cache->counters().byproductInserts);
}

TEST(FilterCacheTest, requestCachesLiveAndDomainCompositionsSeparately) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "cache");
  addTermDoc(writer, "cache");
  addTermDoc(writer, "other");
  writer.commit();
  auto reader = writer.getIndexReader();
  auto cache = writer.getFilterCache();
  std::array identities{FilterCache::SegmentIdentity{
      reader->segments()[0].segInfo.seg_id, reader->segments()[0].maxDoc()}};
  FilterCache::UseRegistry request(*cache, reader->coreGen(), identities);
  auto* use = request.get(FilterKey("domain-composition"));
  use->offerRaw(0, docs(3, {0, 1}), 1);
  auto domain = docs(3, {1, 2});
  auto otherDomain = docs(3, {0, 2});

  DocSet* domainEffective = use->effectiveDocSet(
      0, *reader, domain.get());
  DocSet* otherEffective = use->effectiveDocSet(
      0, *reader, otherDomain.get());
  DocSet* domainAgain = use->effectiveDocSet(
      0, *reader, domain.get());
  DocSet* liveEffective = use->effectiveDocSet(0, *reader);

  EXPECT_EQ(1, domainEffective->card());
  EXPECT_TRUE(domainEffective->get(1));
  EXPECT_EQ(1, otherEffective->card());
  EXPECT_TRUE(otherEffective->get(0));
  EXPECT_EQ(domainEffective, domainAgain);
  EXPECT_EQ(2, liveEffective->card());
}

TEST(FilterCacheTest, rawMaterializationEnforcesExhaustiveFlags) {
  RAMDir dir;
  IndexWriter writer(dir);
  addTermDoc(writer, "cache me");
  writer.commit();
  auto reader = writer.getIndexReader();
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
  SoluxConfig nodeConfig;
  nodeConfig.filterCacheBytes = 0;
  SoluxNode node(nodeConfig);
  solux::test::CollectionHelper helper(node, "filter_cache_reader_counters");
  FilterCacheConfig config = testConfig();
  config.maxEntryBytes = 16;
  config.admissionThreshold = 2;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      solux::test::flatdoc("id", "1", "body_w", "body"),
      solux::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
  auto domains = canonicalDomains(*reader);

  FilterCache::UseRegistry missedRequest(*cache, *reader);
  auto* missed = missedRequest.get(
      FilterKey("small"), FilterKeyScope::READER_STABLE);
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
            missed->probeReaderStable(*reader, domains).kind());
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS,
            missed->probeReaderStable(*reader, domains).kind());
  EXPECT_EQ(2u, cache->counters().misses);
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

TEST(FilterCacheTest, readerValueParticipatesInBenefitDensityEviction) {
  RAMDir dir;
  IndexWriter writer(dir);
  addTermDoc(writer, "first");
  addTermDoc(writer, "second");
  writer.commit();
  auto reader = writer.getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto domains = canonicalDomains(*reader);

  auto publishReader = [&](FilterCache& cache, const FilterKey& key,
                           uint32_t buildCostMicros) {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key, FilterKeyScope::READER_STABLE);
    auto probe = use->probeReaderStable(*reader, domains);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
    return use->publishReaderStable(
        probe, oneDocPerSegment(*reader), buildCostMicros);
  };
  auto publishSegment = [&](FilterCache& cache, const FilterKey& key,
                            uint32_t buildCostMicros) {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key);
    auto probe = use->probe(0);
    EXPECT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    return use->publishRaw(
        0, probe, bitDocs(reader->segments()[0].maxDoc(), 0),
        buildCostMicros);
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
  config.maxEntryBytes = std::max(readerCharge, segmentCharge) + 1;
  config.maxBytes = readerCharge + segmentCharge - 1;
  config.lowWatermarkBytes = std::max(readerCharge, segmentCharge);
  FilterCache cache(config);
  cache.onReaderPublished(*reader);
  FilterKey readerKey("reader-cheap");
  FilterKey segmentKey("segment-expensive");
  auto readerValue = publishReader(cache, readerKey, 1);
  auto segmentValue = publishSegment(
      cache, segmentKey, std::numeric_limits<uint32_t>::max());

  EXPECT_EQ(segmentCharge, cache.bytesUsed());
  EXPECT_EQ(1u, cache.counters().evictions);
  EXPECT_EQ(0u, cache.counters().readerStableRetires);
  EXPECT_NE(nullptr, readerValue);
  EXPECT_NE(nullptr, segmentValue);
  FilterCache::UseRegistry verify(cache, *reader);
  EXPECT_NE(FilterCache::ReaderProbe::Kind::HIT,
            verify.get(readerKey, FilterKeyScope::READER_STABLE)
                ->probeReaderStable(*reader, domains).kind());
  EXPECT_EQ(FilterCache::Probe::Kind::HIT,
            verify.get(segmentKey)->probe(0).kind());
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, readerPublicationRetiresAndRejectsLateKnnValue) {
  SoluxConfig nodeConfig;
  nodeConfig.filterCacheBytes = 0;
  SoluxNode node(nodeConfig);
  solux::test::CollectionHelper helper(node, "filter_cache_reader_retire");
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      solux::test::flatdoc("id", "1", "body_w", "body"),
      solux::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
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

TEST(FilterCacheTest, readerPublishRaceCannotResurrectStaleValue) {
  constexpr int ENTRY_COUNT = 128;
  SoluxConfig nodeConfig;
  nodeConfig.filterCacheBytes = 0;
  SoluxNode node(nodeConfig);
  solux::test::CollectionHelper helper(node, "filter_cache_reader_race");
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxMetadataEntries = ENTRY_COUNT * 2;
  config.maxMetadataBytes = 1024 * 1024;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      solux::test::flatdoc("id", "1", "body_w", "body"),
      solux::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
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
    QueryPrep::DocSetBulkScorer counter(countPool, &source, 192);
    DocSetBuilder builder(192);
    int64_t count = 0;
    EXPECT_EQ(PostingsReader::END, counter.countNextWindow(
        count, &builder, &filter, 0, 192));
    auto counted = builder.build();
    EXPECT_EQ(2, count);
    EXPECT_TRUE(counted->get(65));
    EXPECT_TRUE(counted->get(129));

    MemPool scorePool;
    QueryPrep::DocSetBulkScorer scorer(scorePool, &source, 192);
    ScoreWindow window;
    EXPECT_EQ(PostingsReader::END, scorer.scoreNextWindow(
        window, &filter, 0, 192, std::numeric_limits<float>::lowest()));
    EXPECT_EQ(2, window.size);
    EXPECT_EQ(65, window.docs[0]);
    EXPECT_EQ(129, window.docs[1]);

    MemPool exactPool;
    QueryPrep::DocSetBulkScorer exact(exactPool, &source, 192);
    int64_t exactCount = 0;
    EXPECT_EQ(PostingsReader::END, exact.countNextWindow(
        exactCount, nullptr, nullptr, 0, 192));
    EXPECT_EQ(3, exactCount);
  };

  check(bitset);
  check(array);
}

TEST(FilterCacheIntegrationTest, cachedAndOffMatchAcrossDeleteAndFlush) {
  SoluxConfig onConfig;
  onConfig.filterCacheBytes = 4 * 1024 * 1024;
  SoluxConfig offConfig;
  offConfig.filterCacheBytes = 0;
  SoluxNode onNode(onConfig);
  SoluxNode offNode(offConfig);
  solux::test::CollectionHelper on(onNode, "filter_cache_it");
  solux::test::CollectionHelper off(offNode, "filter_cache_it");

  std::vector<solux::test::Doc> docs;
  docs.reserve(1100);
  for (int32_t i = 0; i < 1100; i++) {
    docs.push_back(solux::test::flatdoc(
        "id", std::to_string(i), "body_w", "body",
        "filter_w", (i & 1) == 0 ? "keep" : "drop",
        "group_s", (i % 3) == 0 ? "a" : "b"));
  }
  ASSERT_TRUE(on.indexAll(docs, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(off.indexAll(docs, UpdateMessage::COMMIT).success);
  auto onCache = on.getIndexWriter()->getFilterCache();
  auto offCache = off.getIndexWriter()->getFilterCache();
  ASSERT_TRUE(onCache->enabled());
  ASSERT_FALSE(offCache->enabled());

  auto registryRequest = solux::test::localReq(offNode.getSearchEngine());
  registryRequest->collection("filter_cache_it")
      .topDocs("registry")
      .matchQuery("body_w", "body")
      .matchFilter("selection", "filter_w", "keep")
      .getNumber()
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

  solux::test::Doc replacement = solux::test::flatdoc(
      "id", "replacement", "body_w", "body", "filter_w", "keep",
      "group_s", "a");
  solux::test::CollectionHelper::UpdateBuilder onUpdate;
  onUpdate.remove("0").add(replacement).commit();
  ASSERT_TRUE(on.submit(onUpdate).success);
  solux::test::CollectionHelper::UpdateBuilder offUpdate;
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
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
  constexpr std::string_view collection = "filter_cache_reset";
  solux::test::CollectionHelper helper(node, collection);
  auto writer = helper.getIndexWriter();
  FilterCacheConfig cacheConfig = testConfig();
  cacheConfig.minSegmentDocs = 1000;
  cacheConfig.admissionThreshold = 1;
  auto firstCache = std::make_shared<FilterCache>(cacheConfig);
  writer->filterCache = firstCache;

  std::vector<solux::test::Doc> firstPhase;
  firstPhase.reserve(1000);
  for (int32_t i = 0; i < 1000; i++) {
    firstPhase.push_back(solux::test::flatdoc(
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
  // This writer was constructed with the cache disabled (filterCacheBytes 0).
  auto autoCache = writer->getFilterCache();
  ASSERT_NE(firstCache, autoCache);
  EXPECT_FALSE(autoCache->enabled());
  // A test that wants cache-on after a reset installs its own again.
  auto secondCache = std::make_shared<FilterCache>(cacheConfig);
  writer->filterCache = secondCache;
  EXPECT_EQ(0u, secondCache->entryCountForTest());

  std::vector<solux::test::Doc> secondPhase;
  std::vector<std::string> expectedIds;
  secondPhase.reserve(1000);
  for (int32_t i = 0; i < 1000; i++) {
    bool keep = i >= 950;
    std::string id = "new-" + std::to_string(i);
    secondPhase.push_back(solux::test::flatdoc(
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
  auto postResetReader = writer->getIndexReader();
  EXPECT_EQ(secondCache.get(), postResetReader->filterCache());
  solux::test::CollectionHelper::UpdateBuilder touch;
  touch.remove("new-0").commit();
  ASSERT_TRUE(helper.submit(touch).success);
  auto successorReader = writer->getIndexReader();
  ASSERT_NE(postResetReader.get(), successorReader.get());
  EXPECT_EQ(secondCache.get(), successorReader->filterCache());
}

TEST(FilterCacheIntegrationTest, membershipProjectionCachesScoreOnlyAndMembershipKnn) {
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
  solux::test::CollectionHelper helper(node, "filter_cache_membership");
  auto writer = helper.getIndexWriter();
  auto cache = std::make_shared<FilterCache>(testConfig());
  writer->filterCache = cache;
  SchemaBuilder schema;
  auto& vector = schema.templ("_v");
  vector.type = api::FieldDef_::FieldClass::VECTOR;
  vector.column = true;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper.collection());

  ASSERT_TRUE(helper.indexAll(std::array{
      solux::test::flatdoc("id", "1", "body_w", "body", "filter_w", "keep",
                           "embedding_v", std::vector<float>{0.0f, 0.0f}),
      solux::test::flatdoc("id", "2", "body_w", "body", "filter_w", "keep",
                           "embedding_v", std::vector<float>{10.0f, 0.0f}),
      solux::test::flatdoc("id", "3", "body_w", "body", "filter_w", "drop",
                           "embedding_v", std::vector<float>{0.0f, 1.0f})},
      UpdateMessage::COMMIT).success);

  auto run = [&](bool requiredTerm) {
    auto request = solux::test::localReq(node.getSearchEngine());
    auto& cursor = request->collection("filter_cache_membership")
                       .topDocs("q")
                       .allQuery()
                       .fields({"id"})
                       .getNumber()
                       .limit(-1);
    auto term = solux::test::qb::match(cursor.mr(), "filter_w", "keep");
    auto knn = solux::test::qb::knn(cursor.mr(), "embedding_v",
                                    {0.0f, 1.0f}, 1, 0, true);
    auto filter = requiredTerm
        ? solux::test::qb::boolean(cursor.mr(), {term}, {knn})
        : solux::test::qb::boolean(cursor.mr(), {}, {term, knn});
    addFilter(cursor, filter);
    request->execute();
    EXPECT_TRUE(request->ok()) << request->toString();
    CachedSearchResult result{
        .count = request->getMatchCount("q"), .ids = {}};
    for (const auto& doc : request->getDocs("q")) {
      auto* id = solux::test::find(doc, "id");
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
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
  solux::test::CollectionHelper helper(node, "filter_cache_knn_refresh");
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->filterCache = cache;

  std::vector<solux::test::Doc> input;
  for (int32_t i = 0; i < 24; i++) {
    input.push_back(solux::test::flatdoc(
        "id", std::to_string(i), "group_s", i < 12 ? "a" : "b",
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  auto firstReader = helper.getIndexWriter()->getIndexReader();
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

  solux::test::CollectionHelper::UpdateBuilder update;
  update.remove("0").commit();
  ASSERT_TRUE(helper.submit(update).success);
  auto secondReader = helper.getIndexWriter()->getIndexReader();
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

TEST(FilterCacheIntegrationTest, knnReaderValueStaysPinnedDuringRetirement) {
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
  constexpr std::string_view collection = "filter_cache_knn_pin";
  solux::test::CollectionHelper helper(node, collection);
  installVectorSchema(helper.collection());
  FilterCacheConfig cacheConfig = testConfig();
  cacheConfig.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  auto writer = helper.getIndexWriter();
  writer->filterCache = cache;

  std::vector<solux::test::Doc> input;
  input.reserve(4096);
  for (int32_t i = 0; i < 4096; i++) {
    input.push_back(solux::test::flatdoc(
        "id", std::to_string(i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);

  constexpr int32_t k = 1024;
  std::array<float, 2> queryVector{0.0f, 0.0f};
  EXPECT_EQ(k, runKnnFilter(
      node, collection, queryVector, k, /*exact=*/true).count);
  EXPECT_EQ(k, runKnnFilter(
      node, collection, queryVector, k, /*exact=*/true).count);
  auto reader = writer->getIndexReader();
  auto schema = helper.collection().getSchema();
  auto* fieldType = dynamic_cast<VectorFieldType*>(
      schema->getFieldTypePtr("embedding_v"));
  ASSERT_NE(nullptr, fieldType);

  AllQuery all;
  KnnQuery knn("embedding_v", *fieldType, queryVector, k, 0, 0, 0.0f,
               /*exact=*/true);
  std::array<Query*, 1> required{&all};
  std::array<Query*, 1> filters{&knn};
  BooleanQuery boolean(required, {}, {}, filters);
  MemPool contextPool;
  FilterKeyContext keyContext;
  keyContext.schemaGen = schema->gen_;
  Query::Context context(
      contextPool, *reader, Query::Context::Limits{}, nullptr, keyContext);
  auto* weight = boolean.createWeight(context, 0);
  ASSERT_TRUE(weight->needsPrepare());
  auto domains = canonicalDomains(*reader);
  Query::Weight::PrepareContext prepareContext{
      *reader, domains, /*parallel=*/false};
  uint64_t hitsBefore = cache->counters().readerStableHits;
  auto prepared = weight->prepare(prepareContext);
  ASSERT_NE(nullptr, prepared);
  ASSERT_EQ(hitsBefore + 1, cache->counters().readerStableHits);

  auto collectPrepared = [&]() -> int64_t {
    int64_t total = 0;
    for (auto& segment : reader->segments()) {
      MemPool pool;
      auto* supplier = prepared->scorerSupplier(pool, segment);
      if (supplier == nullptr) continue;
      auto* scorer = supplier->get(
          pool, std::numeric_limits<int64_t>::max());
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
      if (collectPrepared() != k) collectFailed.store(true);
      rounds.fetch_add(1, std::memory_order_relaxed);
      collecting.count_down();
      signaled = true;
      while (!publicationDone.load(std::memory_order_acquire)) {
        if (collectPrepared() != k) collectFailed.store(true);
        rounds.fetch_add(1, std::memory_order_relaxed);
      }
      // These post-retirement passes make the borrowed-DocSet lifetime bug
      // deterministic under ASan while remaining part of the same request.
      for (int i = 0; i < 8; i++) {
        if (collectPrepared() != k) collectFailed.store(true);
        rounds.fetch_add(1, std::memory_order_relaxed);
      }
    } catch (...) {
      collectFailed.store(true);
      if (!signaled) collecting.count_down();
    }
  });

  collecting.wait();
  solux::test::CollectionHelper::UpdateBuilder update;
  update.remove("0").commit();
  auto updateResult = helper.submit(update);
  std::shared_ptr<IndexReader> nextReader;
  bool readerFailed = false;
  try {
    nextReader = writer->getIndexReader();
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

TEST(FilterCacheIntegrationTest, nestedKnnDomainDoesNotRecordAdmission) {
  SoluxConfig config;
  config.filterCacheBytes = 0;
  SoluxNode node(config);
  solux::test::CollectionHelper helper(node, "filter_cache_knn_gate");
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->filterCache = cache;

  std::vector<solux::test::Doc> input;
  for (int32_t i = 0; i < 20; i++) {
    input.push_back(solux::test::flatdoc(
        "id", std::to_string(i), "group_s", (i & 1) == 0 ? "a" : "b",
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  std::array<float, 2> queryVector{0.0f, 0.0f};

  auto runNested = [&]() {
    auto request = solux::test::localReq(node.getSearchEngine());
    auto& top = request->collection("filter_cache_knn_gate")
                    .topDocs("q").allQuery().limit(0);
    auto& nested = top.facet("groups", "group_s").limit(-1)
                       .topDocs("near").allQuery().limit(-1);
    addFilter(nested, solux::test::qb::knn(
        nested.mr(), "embedding_v", queryVector, 3, 0, true));
    request->execute();
    EXPECT_TRUE(request->ok()) << request->toString();
  };

  runNested();
  runNested();
  auto nestedCounters = cache->counters();
  EXPECT_EQ(0u, nestedCounters.admissions);
  EXPECT_EQ(0u, nestedCounters.builds);
  EXPECT_EQ(0u, cache->entryCountForTest());

  runKnnFilter(node, "filter_cache_knn_gate", queryVector, 3);
  EXPECT_EQ(0u, cache->counters().admissions)
      << "nested domains must not count as the first admission sighting";
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
  SoluxConfig config;
  config.filterCacheBytes = 4 * 1024 * 1024;
  SoluxNode node(config);
  solux::test::CollectionHelper helper(node, "filter_cache_mixed");
  std::vector<solux::test::Doc> input;
  input.reserve(1100);
  for (int32_t i = 0; i < 1100; i++) {
    input.push_back(solux::test::flatdoc(
        "id", std::to_string(i), "body_w", "body",
        "filter_w", i < 30 ? "sparse" : "other"));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  auto cache = helper.getIndexWriter()->getFilterCache();
  EXPECT_EQ(30, runCachedSearch(node, "filter_cache_mixed", "sparse").count);
  EXPECT_EQ(30, runCachedSearch(node, "filter_cache_mixed", "sparse").count);

  solux::test::CollectionHelper::UpdateBuilder update;
  for (int32_t i = 0; i < 20; i++) update.remove(std::to_string(i));
  update.commit();
  ASSERT_TRUE(helper.submit(update).success);
  auto before = cache->counters();

  EXPECT_EQ(10, runCachedSearch(node, "filter_cache_mixed", "sparse").count);
  EXPECT_GT(cache->counters().hits, before.hits);
}

TEST(FilterCacheIntegrationTest, multiSelectFacetMaterializationCapturesByproducts) {
  SoluxConfig config;
  config.filterCacheBytes = 4 * 1024 * 1024;
  SoluxNode node(config);
  solux::test::CollectionHelper helper(node, "filter_cache_facet");
  std::vector<solux::test::Doc> docs;
  docs.reserve(1100);
  for (int32_t i = 0; i < 1100; i++) {
    docs.push_back(solux::test::flatdoc(
        "id", std::to_string(i), "body_w", "body",
        "filter_w", (i & 1) == 0 ? "keep" : "drop",
        "group_s", (i % 3) == 0 ? "a" : "b"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto run = [&]() {
    auto request = solux::test::localReq(node.getSearchEngine());
    auto& top = request->collection("filter_cache_facet")
                    .topDocs("q")
                    .matchQuery("body_w", "body")
                    .matchFilter("selection", "filter_w", "keep")
                    .matchFilter("group", "group_s", "a")
                    .getNumber()
                    .limit(0);
    top.facet("groups", "group_s").limit(-1);
    FilterFoldGuard passive(true);
    request->execute();
    EXPECT_TRUE(request->ok()) << request->toString();
    return request->getMatchCount("q");
  };

  EXPECT_EQ(184, run());
  EXPECT_EQ(184, run());
  EXPECT_GE(helper.getIndexWriter()->getFilterCache()
                ->counters().byproductInserts, 2u);
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
  builder.appendTag(FilterKeyTag::CONSTANT_SCORE);
  builder.appendFloat(std::bit_cast<float>(bits));
  return std::move(builder)
      .finish(FilterKeyScope::SEGMENT_STABLE, {})
      .value();
}

class KeyOnlyTermQuery final : public Query {
  std::string_view term;

public:
  explicit KeyOnlyTermQuery(std::string_view term) : term(term) {}

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::TERM);
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

TEST(FilterKeyTest, dateMathKeysFollowCoercedBuckets) {
  TimeZone utc = TimeZone::utc();
  int64_t morning = *parseDateToEpochMillis("2026-07-22T08:00:00Z");
  int64_t evening = *parseDateToEpochMillis("2026-07-22T22:00:00Z");
  int64_t tomorrow = *parseDateToEpochMillis("2026-07-23T08:00:00Z");
  DateRange first = *parseDateRange("NOW/DAY", morning, utc);
  DateRange same = *parseDateRange("NOW/DAY", evening, utc);
  DateRange next = *parseDateRange("NOW/DAY", tomorrow, utc);

  NumericRangeQuery firstQuery("when", first.lo, first.hiExclusive - 1);
  NumericRangeQuery sameQuery("when", same.lo, same.hiExclusive - 1);
  NumericRangeQuery nextQuery("when", next.lo, next.hiExclusive - 1);
  FilterKeyContext ctx{.schemaGen = 7, .timeZone = "UTC"};

  EXPECT_EQ(keyFor(firstQuery, ctx), keyFor(sameQuery, ctx));
  EXPECT_NE(keyFor(firstQuery, ctx), keyFor(nextQuery, ctx));
}

TEST(FilterKeyTest, discriminatesTimezoneAndSchemaGeneration) {
  NumericRangeQuery query("when", 100, 200);
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
  struct PolicyRestore {
    int64_t candidates = KnnQuery::maxKnnCandidates;
    int32_t breadth = KnnQuery::maxKnnBreadth;
    int32_t refineCount = KnnQuery::defaultAnnRefineCount;
    int32_t refineRatio = KnnQuery::defaultAnnRefineRatio;
    decltype(KnnQuery::engineWrapperForTests) wrapper =
        KnnQuery::engineWrapperForTests;
    ~PolicyRestore() {
      KnnQuery::maxKnnCandidates = candidates;
      KnnQuery::maxKnnBreadth = breadth;
      KnnQuery::defaultAnnRefineCount = refineCount;
      KnnQuery::defaultAnnRefineRatio = refineRatio;
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

  KnnQuery::maxKnnCandidates++;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::maxKnnCandidates = restore.candidates;
  KnnQuery::maxKnnBreadth++;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::maxKnnBreadth = restore.breadth;
  KnnQuery::defaultAnnRefineCount++;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::defaultAnnRefineCount = restore.refineCount;
  KnnQuery::defaultAnnRefineRatio++;
  EXPECT_NE(baseKey, keyFor(base));
  KnnQuery::defaultAnnRefineRatio = restore.refineRatio;

  KnnQuery::engineWrapperForTests = [](VectorEngine&, int64_t) {
    return std::unique_ptr<VectorEngine>();
  };
  EXPECT_FALSE(optionalKeyFor(base).has_value());
}

TEST(FilterKeyTest, fuzzyIsCoreStableAndIncludesEffectiveLimit) {
  FuzzyQuery fuzzy("title", "solux", 2, 1, 0);
  FilterKeyContext first{.schemaGen = 5, .coreGen = 10,
                         .fuzzyMaxExpansions = 40, .timeZone = "UTC"};
  FilterKeyContext otherCore = first;
  otherCore.coreGen = 11;
  FilterKeyContext otherLimit = first;
  otherLimit.fuzzyMaxExpansions = 30;

  EXPECT_NE(keyFor(fuzzy, first), keyFor(fuzzy, otherCore));
  EXPECT_NE(keyFor(fuzzy, first), keyFor(fuzzy, otherLimit));
}

TEST(FilterKeyTest, everyConcreteQueryMakesAnExplicitScopeDecision) {
  AllQuery all;
  MatchNoDocsQuery none;
  TermQuery term("f", "v");
  std::array<std::string_view, 2> phraseTerms{"a", "b"};
  std::array<int32_t, 2> phrasePositions{0, 1};
  PhraseQuery phrase("f", phraseTerms, phrasePositions, 0);
  ExistsQuery exists("f");
  NumericRangeQuery numeric("n", 1, 2);
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
using namespace solux::test;

namespace {

constexpr int32_t DOC_UNIVERSE = 96;

enum class FilterKind : uint8_t {
  DENSE_TERM,
  SPARSE_TERM,
  NUMERIC_RANGE,
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
    case FilterKind::NUMERIC_RANGE: {
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
    case FilterKind::NUMERIC_RANGE:
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
  auto* named = api::build::allocArray(top.filter, 1, cursor.mr());
  named[0].name = "selection";
  auto* stored = (api::Query*)cursor.mr().allocate(sizeof(api::Query),
                                                    alignof(api::Query));
  new (stored) api::Query(query);
  named[0].query = stored;
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
                     .getNumber()
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
    out << "count oracle: get_number=" << result.count
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
  helper.getIndexWriter()->updateGraph.wait_for_all();
  helper.getIndexWriter()->getIndexReader();
  return result;
}

void validateConcurrentByproductPublication() {
  constexpr int threadCount = 16;
  constexpr int rounds = 8;
  FilterCacheConfig config{
      .maxBytes = 1024 * 1024,
      .lowWatermarkBytes = 900 * 1024,
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
      .lowWatermarkBytes = 900 * 1024,
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
#ifdef SOLUX_ASAN
  constexpr int entries = 8192;
#else
  constexpr int entries = 32768;
#endif
  FilterCacheConfig config{
      .maxBytes = 64 * 1024 * 1024,
      .lowWatermarkBytes = 0,
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

class FilterCacheConcurrencyTest : public SoluxTest {};

TEST_F(FilterCacheConcurrencyTest, updateMergePurgeEvictFuzz) {
  ASSERT_NO_THROW(validateConcurrentByproductPublication());
  ASSERT_NO_THROW(validateStalePublicationRejection());
  ASSERT_NO_THROW(validateConcurrentSweepPurgeAccounting());

  constexpr std::string_view collection = "filter_cache_concurrency";
  const int writerThreads = 3;
  const int queryThreads = 4;
  const int phases = 3;
  const int percentDeletes = 35;
  const int percentCommits = 70;
#ifdef SOLUX_ASAN
  const int batchesPerWriter = 18;
#else
  const int batchesPerWriter = 40;
#endif

  SoluxConfig nodeConfig;
  nodeConfig.filterCacheBytes = 0;
  SoluxNode node(nodeConfig);
  CollectionHelper helper(node, collection);
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(3);
  writer->perInverterMaxDocs = 4;
  writer->perInverterRamBytes = 16 * 1024;

  FilterCacheConfig cacheConfig{
      .maxBytes = 768,
      .lowWatermarkBytes = 384,
      .maxEntryBytes = 512,
      .minSegmentDocs = 0,
      .admissionHistorySize = 64,
      .admissionThreshold = 2,
      .maxMetadataEntries = 32,
      .maxMetadataBytes = 16 * 1024};
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  writer->filterCache = cache;

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
            all.ids.size());
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
