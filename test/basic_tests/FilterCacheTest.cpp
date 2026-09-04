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

#include "luxir/index/IndexWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
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
#include "luxir/store/Directory.h"
#include "luxir/util/DateTime.h"
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
      .getNumber()
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
                     .getNumber()
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
    const FilterKey& key, int32_t expectedDoc, uint32_t buildCostMicros,
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
        0, probe, bitDocs(4096, expectedDoc), buildCostMicros);
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
// keeps admission-lane sightings, so a known key re-admits and rebuilds
// immediately (eviction refill); clear() + resetAdmission() restarts the
// whole observation ladder (first sighting). dump() reports resident entries
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
  EXPECT_GT(rows[0].hits, 0u);

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
  IndexWriter writer(dir);
  addTermDoc(writer, "first");
  addTermDoc(writer, "second");
  writer.commit();
  auto reader = writer.getIndexReader();
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
    DocSet* accepted = acceptedUse->effectiveDocSet(
        (size_t) segment.ord, *reader,
        domains[(size_t) segment.ord]);
    ASSERT_NE(nullptr, accepted);
    EXPECT_EQ(1, accepted->card());
  }
}

TEST(FilterCacheTest, existingReaderAcceptanceRejectsPublishedReader) {
  RAMDir dir;
  IndexWriter writer(dir);
  addTermDoc(writer, "first");
  writer.commit();
  auto reader = writer.getIndexReader();
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
  auto nextReader = writer.getIndexReader();
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

TEST(FilterCacheTest, existingAcceptanceResetsDetachDebt) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;
  config.lowWatermarkBytes = charge;
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
  ASSERT_EQ(1u, cache.counters().capacityDeadBuilds);

  auto* acceptedUse = request.acceptExisting(
      std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
  ASSERT_NE(nullptr, acceptedUse);
  auto accepted = acceptedUse->probe(0);
  ASSERT_EQ(FilterCache::Probe::Kind::HIT, accepted.kind());
  ASSERT_NE(nullptr, accepted.docSet());
  EXPECT_TRUE(accepted.docSet()->get(7));

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

TEST(FilterCacheTest, zeroHitEvictionBackoffBoundsOrderedReplay) {
  auto sample = bitDocs(4096, 1);
  size_t charge = sample->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 3 - 1;
  config.lowWatermarkBytes = charge * 2;
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

  constexpr int logarithmicBuildBound = 1 + (int)std::bit_width(63U);
  int totalDeadBuilds = 0;
  for (int i = 0; i < deadCount; i++) {
    EXPECT_LE(claimedBuilds[i],
              logarithmicBuildBound + (sharedHits[i] ? 1 : 0));
    totalDeadBuilds += claimedBuilds[i];
  }
  auto counters = cache.counters();
  EXPECT_GT(counters.thrashBuildSkips, 0u);
  EXPECT_EQ(2u + (uint64_t)totalDeadBuilds, counters.buildAttempts);
  EXPECT_EQ((uint64_t)totalDeadBuilds, counters.capacityDeadBuilds);
  EXPECT_LT(cache.bytesUsed(), charge * deadCount / 2);
  for (int i = 0; i < (int)hotKeys.size(); i++) {
    EXPECT_EQ(FilterCache::Probe::Kind::HIT,
              servePolicyValue(cache, segments, hotKeys[i], i + 1, 100'000));
  }
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, evictionBackoffRecoversAndHitResetsDebt) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;
  config.lowWatermarkBytes = charge;
  FilterCache cache(config);
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  ASSERT_TRUE(cache.onReaderPublished(1, segments));
  FilterKey winner("phase-winner");
  FilterKey recovering("phase-recovering");

  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, winner, 1, 10));
  EXPECT_EQ(FilterCache::Probe::Kind::HIT,
            servePolicyValue(cache, segments, winner, 1, 10));
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, recovering, 2, 1));
  EXPECT_EQ(1u, cache.counters().capacityDeadBuilds);
  EXPECT_EQ(FilterCache::Probe::Kind::BYPASS,
            servePolicyValue(cache, segments, recovering, 2, 1));

  bool recovered = false;
  for (int i = 0; i < 4096 && !recovered; i++) {
    auto kind = servePolicyValue(cache, segments, recovering, 2, 1);
    recovered = kind == FilterCache::Probe::Kind::HIT;
  }
  ASSERT_TRUE(recovered);

  uint64_t deadBuildsBeforeHitEviction = cache.counters().capacityDeadBuilds;
  FilterKey replacement("phase-replacement");
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, replacement, 3, 100'000));
  EXPECT_EQ(deadBuildsBeforeHitEviction,
            cache.counters().capacityDeadBuilds);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, recovering, 2, 1));
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, byproductEvictionDoesNotAccrueDeadBuildDebt) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;
  config.lowWatermarkBytes = charge;
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
  EXPECT_EQ(0u, cache.counters().capacityDeadBuilds);
  EXPECT_EQ(0u, cache.counters().thrashBuildSkips);
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD,
            servePolicyValue(cache, segments, byproduct, 1, 1));
  ASSERT_NO_THROW(cache.validateForTest());
}

TEST(FilterCacheTest, purgeAndMetadataSweepDiscardBackoffGhosts) {
  size_t charge = bitDocs(4096, 1)->ramBytesUsed();
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  config.maxEntryBytes = charge + 1;
  config.maxBytes = charge * 2 - 1;
  config.lowWatermarkBytes = charge;
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
  ASSERT_EQ(1u, cache.counters().capacityDeadBuilds);
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

  uint64_t skipsBefore = cache.counters().thrashBuildSkips;
  FilterCache::UseRegistry retry(cache, 2, newSegments);
  auto* retryUse = retry.get(victim);
  EXPECT_NE(nullptr, retryUse->entryIdentityForTest());
  EXPECT_EQ(FilterCache::Probe::Kind::BUILD, retryUse->probe(0).kind());
  EXPECT_EQ(skipsBefore, cache.counters().thrashBuildSkips);
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
  config.lowWatermarkBytes = 1;
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
  cache.sweep();
  ASSERT_EQ(1u, cache.counters().capacityDeadBuilds);
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
  ASSERT_NO_THROW(cache.validateForTest());
  EXPECT_GT(cache.counters().buildAttempts, 0u);
  EXPECT_GT(cache.counters().capacityDeadBuilds, 0u);
  EXPECT_GT(cache.counters().thrashBuildSkips, 0u);
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
      execPool, *weight, context.getFilterUse(query), *reader,
      reader->segments()[0],
      QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE);
  ASSERT_NE(nullptr, supplier);
  EXPECT_EQ(nullptr, dynamic_cast<QueryPrep::DocSetSupplier*>(supplier));
}

TEST(FilterCacheTest, weightConstantCountIsNarrowAndTransparent) {
  RAMDir dir;
  IndexWriter writer(dir);
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "alpha");
  addTermDoc(writer, "beta");
  writer.commit();
  auto reader = writer.getIndexReader();
  auto& segment = reader->segments()[0];
  MemPool pool;
  Query::Context context(pool, *reader);

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

  EXPECT_EQ(3, *allWeight->constantCount(segment, nullptr));
  EXPECT_EQ(0, *noneWeight->constantCount(segment, nullptr));
  EXPECT_EQ(2, *alphaWeight->constantCount(segment, nullptr));
  EXPECT_EQ(2, *constantWeight->constantCount(segment, nullptr));
  EXPECT_EQ(2, *boostWeight->constantCount(segment, nullptr));
  EXPECT_EQ(2, *preparedWeight->constantCount(segment, nullptr));

  auto restricted = docs(3, {0, 2});
  EXPECT_FALSE(allWeight->constantCount(
      segment, restricted.get()).has_value());
  EXPECT_EQ(0, *noneWeight->constantCount(segment, restricted.get()));
  EXPECT_FALSE(alphaWeight->constantCount(
      segment, restricted.get()).has_value());

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
  EXPECT_EQ(2, *deadClauseUnionWeight->constantCount(segment, nullptr));
  EXPECT_FALSE(liveUnionWeight->constantCount(segment, nullptr).has_value());
  EXPECT_EQ(0, *deadUnionWeight->constantCount(segment, nullptr));
  EXPECT_EQ(0, *deadUnionWeight->constantCount(segment, restricted.get()));
  EXPECT_EQ(2, *mandOptWeight->constantCount(segment, nullptr));
  EXPECT_FALSE(gatedOptWeight->constantCount(segment, nullptr).has_value());
  EXPECT_FALSE(excludedWeight->constantCount(segment, nullptr).has_value());
  EXPECT_FALSE(deadClauseUnionWeight->constantCount(
      segment, restricted.get()).has_value());

  ExistsQuery exists("text_w");
  ExistsQuery absentField("other_w");
  auto* existsWeight = exists.createWeight(context, 0);
  auto* absentFieldWeight = absentField.createWeight(context, 0);
  EXPECT_EQ(3, *existsWeight->constantCount(segment, nullptr));
  EXPECT_EQ(0, *absentFieldWeight->constantCount(segment, nullptr));
  EXPECT_EQ(0, *absentFieldWeight->constantCount(segment, restricted.get()));
  EXPECT_FALSE(existsWeight->constantCount(
      segment, restricted.get()).has_value());
}

TEST(FilterCacheTest,
     wholeMembershipComposesIncomingDomainAndGuardsPublication) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "alpha");
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "beta");
  addTermDoc(writer, "alpha beta gamma");
  writer.commit();
  auto reader = writer.getIndexReader();
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
  auto built = firstPlan.resolve(*reader, segment, incoming.get());
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
  auto hit = hitPlan.resolve(*reader, segment, incoming.get());
  ASSERT_TRUE(hit.available);
  EXPECT_EQ(1, hit.count);
  EXPECT_EQ(1, SkipStats::wholeCountHits);
  EXPECT_EQ(hit.docs.get(), hitUse->effectiveDocSet(
      0, *reader, incoming.get()));

  auto emptyDomain = docs(5, {});
  auto allDeleted = hitPlan.resolve(
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
      dependentPlan.resolve(*reader, segment, nullptr), std::logic_error);

  MemPool retryPool;
  Query::Context retryContext(retryPool, *reader);
  auto* retryWeight = dependentQuery.createWeight(retryContext, 0);
  auto* retryUse = retryContext.getFilterUse(
      dependentQuery, FilterCache::AdmissionLane::WHOLE);
  QueryPrep::WholeMembershipPlan retryPlan(
      *retryWeight, retryUse, retryContext.filterUses);
  auto retry = retryPlan.resolve(*reader, segment, nullptr);
  ASSERT_TRUE(retry.available);
  EXPECT_EQ(1, retry.count);
}

TEST(FilterCacheTest, wholeAdmissionDiscriminatesSchemaGeneration) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "alpha beta");
  addTermDoc(writer, "alpha");
  writer.commit();
  auto reader = writer.getIndexReader();
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
  cursor.getNumber().withStats().fields({"id"}).limit(100);
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
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "a keep");
  addTermDoc(writer, "a");
  addTermDoc(writer, "keep");
  writer.commit();
  auto reader = writer.getIndexReader();

  TermQuery term("text_w", "a");
  ForcePrepareQuery forcedTerm(&term);
  MemPool canonicalPool;
  Query::Context canonicalContext(canonicalPool, *reader);
  auto* canonicalWeight = forcedTerm.createWeight(canonicalContext, 0);
  std::array<Query::Weight*, 1> canonicalWeights{canonicalWeight};
  std::array<FilterCache::Use*, 1> canonicalUses{
      canonicalContext.getFilterUse(forcedTerm)};
  Query::Weight::PrepareContext canonicalPrepare{
      *reader, std::span<DocSet* const>{}, false};
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
      *reader, std::span<DocSet* const>{}, false};
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
      *racerWeight, racerUse, *reader, reader->segments()[0], nullptr);

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

TEST(FilterCacheTest, recursiveRawBuildUsesRawHitsAcrossBooleanFilterModes) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addIdTermDoc(writer, "0", "body selected alpha");
  addIdTermDoc(writer, "1", "body selected beta");
  addIdTermDoc(writer, "2", "body alpha");
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("0", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = writer.getIndexReader(0);
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
    auto before = writer.getFilterCache()->counters();
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
    auto after = writer.getFilterCache()->counters();
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
  IndexWriter writer(dir, {}, nullptr, config);
  addIdTermDoc(writer, "0", "body selected");
  addIdTermDoc(writer, "1", "body selected");
  addIdTermDoc(writer, "2", "body");
  writer.commit();
  auto& deletes = writer.obtainInverter();
  deletes.deleteId("0", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto reader = writer.getIndexReader(0);
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

  auto hitsBefore = writer.getFilterCache()->counters().hits;
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
  EXPECT_GT(writer.getFilterCache()->counters().hits, hitsBefore);
}

TEST(FilterCacheTest, preparedProhibitedSourceBuildsThenHits) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "body selected");
  addTermDoc(writer, "body");
  addTermDoc(writer, "body selected");
  writer.commit();
  auto reader = writer.getIndexReader(0);
  auto& segment = reader->segments()[0];

  auto run = [&]() {
    MemPool pool;
    Query::Context context(pool, *reader);
    TermQuery body("text_w", "body");
    TermQuery selected("text_w", "selected");
    ForcePrepareQuery preparedSelected(&selected);
    std::array<Query*, 1> mandatory{&body};
    std::array<Query*, 1> prohibited{&preparedSelected};
    BooleanQuery query(mandatory, {}, prohibited, {});
    auto* weight = query.createWeight(context, 0);
    Query::Weight::PrepareContext prepareContext{
        *reader, std::span<DocSet* const>{}, false};
    auto prepared = weight->prepare(prepareContext);
    if (prepared == nullptr) {
      ADD_FAILURE() << "Boolean prohibited preparation returned null";
      return std::unique_ptr<DocSet>();
    }
    return QueryPrep::materialize(*prepared, segment, nullptr);
  };

  SkipStatsScope stats;
  auto built = run();
  ASSERT_NE(nullptr, built);
  EXPECT_EQ(1, built->card());
  EXPECT_EQ(1, SkipStats::prohibitedCachePullBuilds);
  SkipStats::reset();
  auto hit = run();
  ASSERT_NE(nullptr, hit);
  EXPECT_EQ(1, hit->card());
  EXPECT_EQ(1, SkipStats::prohibitedCachePullHits);
}

TEST(FilterCacheTest, recursiveRawPublicationServesPinnedOldReader) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addIdTermDoc(writer, "0", "body selected");
  addIdTermDoc(writer, "1", "body selected");
  addIdTermDoc(writer, "2", "body");
  writer.commit();
  auto oldReader = writer.getIndexReader(0);
  ASSERT_EQ(1u, oldReader->segments().size());
  ASSERT_EQ(nullptr, oldReader->segments()[0].liveDocs());

  auto& deletes = writer.obtainInverter();
  deletes.deleteId("0", 1);
  writer.releaseInverter(deletes);
  writer.commit();
  auto newReader = writer.getIndexReader(0);
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

  auto beforeOld = writer.getFilterCache()->counters();
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
    EXPECT_GT(writer.getFilterCache()->counters().hits, beforeOld.hits);
  }
}

TEST(FilterCacheTest, recursiveRawBuildExceptionReleasesBothClaims) {
  FilterCacheConfig config = testConfig();
  config.admissionThreshold = 1;
  RAMDir dir;
  IndexWriter writer(dir, {}, nullptr, config);
  addTermDoc(writer, "body selected");
  addTermDoc(writer, "body");
  writer.commit();
  auto reader = writer.getIndexReader(0);

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
  EXPECT_EQ(0u, writer.getFilterCache()->counters().builds);

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
  helper.getIndexWriter()->filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
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

TEST(FilterCacheTest, readerZeroHitEvictionBacksOffAndHitResets) {
  LuxirConfig nodeConfig;
  nodeConfig.queryCacheBytes = 0;
  LuxirNode node(nodeConfig);
  luxir::test::CollectionHelper helper(node, "filter_cache_reader_backoff");
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
      UpdateMessage::COMMIT).success);
  auto writer = helper.getIndexWriter();
  auto reader = writer->getIndexReader();
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
  config.lowWatermarkBytes = config.maxBytes;
  auto cache = std::make_shared<FilterCache>(config);
  writer->filterCache = cache;
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
  ASSERT_EQ(1u, cache->counters().readerDeadBuilds);

  for (int bypass = 0; bypass < 2; bypass++) {
    FilterCache::UseRegistry request(*cache, *reader);
    auto probe = request.get(victim, FilterKeyScope::READER_STABLE)
        ->probeReaderStable(*reader, domains);
    EXPECT_EQ(FilterCache::ReaderProbe::Kind::BYPASS, probe.kind());
  }
  ASSERT_EQ(2u, cache->counters().readerThrashBuildSkips);

  std::vector<std::string> deletes{"1"};
  helper.deleteByIds(deletes, UpdateMessage::COMMIT);
  auto nextReader = writer->getIndexReader();
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

  publish(*nextReader, FilterKey("reader-evictor"),
          std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(1u, cache->counters().readerDeadBuilds)
      << "a shared HIT must prevent new zero-hit debt";
  FilterCache::UseRegistry retryRequest(*cache, *nextReader);
  auto retry = retryRequest.get(victim, FilterKeyScope::READER_STABLE)
      ->probeReaderStable(*nextReader, nextDomains);
  EXPECT_EQ(FilterCache::ReaderProbe::Kind::BUILD, retry.kind());
  EXPECT_EQ(2u, cache->counters().readerThrashBuildSkips);
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
  helper.getIndexWriter()->filterCache = cache;
  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 8; i++) {
    input.push_back(luxir::test::flatdoc(
        "id", std::to_string(i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  ASSERT_TRUE(helper.indexAll(input, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
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
        *reader, nestedDomains, /*parallel=*/false};
    auto prepared = plan.prepareMainWeight(prepareContext);
    ASSERT_NE(nullptr, prepared);
    auto result = plan.resolve(
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
        *reader, rootDomains, /*parallel=*/false};
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
  helper.getIndexWriter()->filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
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

TEST(FilterCacheTest, routedAccountingOwnsRejectedReaderValue) {
  RAMDir dir;
  IndexWriter writer(dir);
  addTermDoc(writer, "body");
  writer.commit();
  auto reader = writer.getIndexReader();
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
  helper.getIndexWriter()->filterCache = cache;
  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "body"),
      luxir::test::flatdoc("id", "2", "body_w", "body")},
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
  auto onCache = on.getIndexWriter()->getFilterCache();
  auto offCache = off.getIndexWriter()->getFilterCache();
  ASSERT_TRUE(onCache->enabled());
  ASSERT_FALSE(offCache->enabled());

  auto registryRequest = luxir::test::localReq(offNode.getSearchEngine());
  registryRequest->collection("filter_cache_it")
      .topDocs("registry")
      .matchQuery("body_w", "body")
      .matchFilter("filter_w", "keep")
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
  writer->filterCache = firstCache;

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
  auto autoCache = writer->getFilterCache();
  ASSERT_NE(firstCache, autoCache);
  EXPECT_FALSE(autoCache->enabled());
  // A test that wants cache-on after a reset installs its own again.
  auto secondCache = std::make_shared<FilterCache>(cacheConfig);
  writer->filterCache = secondCache;
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
  auto postResetReader = writer->getIndexReader();
  EXPECT_EQ(secondCache.get(), postResetReader->filterCache());
  luxir::test::CollectionHelper::UpdateBuilder touch;
  touch.remove("new-0").commit();
  ASSERT_TRUE(helper.submit(touch).success);
  auto successorReader = writer->getIndexReader();
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
  writer->filterCache = cache;
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
                       .getNumber()
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
  helper.getIndexWriter()->filterCache = cache;

  std::vector<luxir::test::Doc> input;
  for (int32_t i = 0; i < 24; i++) {
    input.push_back(luxir::test::flatdoc(
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

  luxir::test::CollectionHelper::UpdateBuilder update;
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

TEST(FilterCacheIntegrationTest, fuzzyBoostsShareWholeCountEntry) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_fuzzy_boost";
  luxir::test::CollectionHelper helper(node, collection);
  FilterCacheConfig cacheConfig = testConfig();
  cacheConfig.admissionThreshold = 1;
  cacheConfig.minSegmentDocs = 0;
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  helper.getIndexWriter()->filterCache = cache;

  ASSERT_TRUE(helper.indexAll(std::array{
      luxir::test::flatdoc("id", "1", "body_w", "color"),
      luxir::test::flatdoc("id", "2", "body_w", "colon"),
      luxir::test::flatdoc("id", "3", "body_w", "colors"),
      luxir::test::flatdoc("id", "4", "body_w", "other")},
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
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
        *weight, use, context.filterUses);
    int64_t result = 0;
    for (auto& segment : reader->segments()) {
      DocSet* domain = segment.liveDocs() == nullptr
          ? nullptr : &segment.liveDocs()->docset();
      auto resolved = plan.resolve(*reader, segment, domain);
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
  EXPECT_GT(afterBoosted.hits, afterPlain.hits);
}

TEST(FilterCacheIntegrationTest, cacheFirstKnnCountOmitsWeightAndPrepare) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  constexpr std::string_view collection = "filter_cache_knn_count_first";
  luxir::test::CollectionHelper helper(node, collection);
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->filterCache = cache;

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
                       .topDocs("q").getNumber().limit(0);
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
  helper.getIndexWriter()->filterCache = cache;

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
    auto& nested = root.topDocs("near").getNumber().limit(0);
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
  writer->filterCache = cache;

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
  auto reader = writer->getIndexReader();
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
        *reader, domains, /*parallel=*/false};
    auto buildPrepared = buildPlan.prepareMainWeight(buildPrepareContext);
    int64_t built = 0;
    for (auto& segment : reader->segments()) {
      auto result = buildPlan.resolve(
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
      *reader, domains, /*parallel=*/false};
  uint64_t hitsBefore = cache->counters().readerStableHits;
  auto prepared = plan.prepareMainWeight(prepareContext);
  ASSERT_NE(nullptr, prepared);
  ASSERT_EQ(hitsBefore + 1, cache->counters().readerStableHits);

  std::vector<DomainHandle> cachedMembership;
  cachedMembership.reserve(reader->segments().size());
  int64_t cachedCount = 0;
  for (auto& segment : reader->segments()) {
    auto result = plan.resolve(
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

TEST(FilterCacheIntegrationTest, rejectedNestedKnnDoesNotRecordAdmission) {
  LuxirConfig config;
  config.queryCacheBytes = 0;
  LuxirNode node(config);
  luxir::test::CollectionHelper helper(node, "filter_cache_knn_gate");
  installVectorSchema(helper.collection());
  auto cache = std::make_shared<FilterCache>(testConfig());
  helper.getIndexWriter()->filterCache = cache;

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
    EXPECT_FALSE(request->ok()) << request->toString();
    EXPECT_NE(request->errorMsg().find("cannot emit per bucket"),
              std::string::npos);
  };

  runNested();
  runNested();
  auto nestedCounters = cache->counters();
  EXPECT_EQ(0u, nestedCounters.admissions);
  EXPECT_EQ(0u, nestedCounters.builds);
  EXPECT_EQ(0u, cache->entryCountForTest());

  runKnnFilter(node, "filter_cache_knn_gate", queryVector, 3);
  EXPECT_EQ(0u, cache->counters().admissions)
      << "rejected nested requests must not count as an admission sighting";
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
  auto cache = helper.getIndexWriter()->getFilterCache();
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
                    .getNumber()
                    .limit(0);
    top.facet("groups", "group_s").limit(-1);
    FilterFoldGuard passive(true);
    request->execute();
    EXPECT_TRUE(request->ok()) << request->toString();
    return request->getMatchCount("q");
  };

  EXPECT_EQ(184, run());
  auto beforeBuild = helper.getIndexWriter()->getFilterCache()->counters();
  EXPECT_EQ(184, run());
  auto afterBuild = helper.getIndexWriter()->getFilterCache()->counters();
  EXPECT_GE(afterBuild.builds - beforeBuild.builds, 2u);
  auto beforeHit = afterBuild;
  EXPECT_EQ(184, run());
  EXPECT_GE(helper.getIndexWriter()->getFilterCache()->counters().hits
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
  FuzzyQuery fuzzy("title", "luxir", 2, 1, 0);
  FuzzyQuery boostedFuzzy("title", "luxir", 2, 1, 0, 3.0f);
  BoostQuery wrappedFuzzy(&fuzzy, 3.0f);
  FilterKeyContext first{.schemaGen = 5, .coreGen = 10,
                         .fuzzyMaxExpansions = 40, .timeZone = "UTC"};
  FilterKeyContext otherCore = first;
  otherCore.coreGen = 11;
  FilterKeyContext otherLimit = first;
  otherLimit.fuzzyMaxExpansions = 30;

  EXPECT_NE(keyFor(fuzzy, first), keyFor(fuzzy, otherCore));
  EXPECT_NE(keyFor(fuzzy, first), keyFor(fuzzy, otherLimit));
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
#ifdef LUXIR_ASAN
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

class FilterCacheConcurrencyTest : public LuxirTest {};

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
