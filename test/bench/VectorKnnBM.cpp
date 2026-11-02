// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "test/KnnTestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace api = luxir::api;

namespace {

// Benchmark codec selector (the "codec" benchmark arg): the field's schema
// ann settings, so the production path is what gets measured.
enum BenchCodec : int32_t {
  RABITQ1 = 0,  // explicit RaBitQ: 1-bit codes, 4-bit queries
  PQ = 1,       // IVF+PQ with its defaults (M <= 32, 8 bits)
  RABITQ4 = 2,  // extended RaBitQ, 4 bits per dimension
  RABITQ2 = 3,
  PQ_D8 = 4,    // IVF+PQ, dims/8 sub-quantizers: one code byte per 8 dims,
                // about the bytes of 1-bit RaBitQ
};

const char* benchCodecName(int32_t codec) {
  switch (codec) {
    case RABITQ1: return "rabitq1";
    case PQ: return "pq";
    case RABITQ4: return "rabitq4";
    case RABITQ2: return "rabitq2";
    case PQ_D8: return "pq_d8";
    default: return "?";
  }
}

int32_t benchRaBitQBits(int32_t codec) {
  return codec == RABITQ4 ? 4 : codec == RABITQ2 ? 2 : 1;
}

void setBenchAnn(api::FieldDef& f, int32_t dims, int32_t codec) {
  if (codec == PQ || codec == PQ_D8) {
    f.ann = pqAnn(luxir::unit_tests ? 4 : 0,
                  luxir::unit_tests ? 2 : codec == PQ_D8 ? dims / 8 : 0,
                  luxir::unit_tests ? 1 : 8);
  } else {
    f.ann = rabitqAnn(luxir::unit_tests ? 4 : 0, benchRaBitQBits(codec));
  }
}

void installVectorBenchSchema(Collection& col, int32_t dims,
                              api::VectorMetric metric = api::VectorMetric::IP,
                              int32_t codec = RABITQ1) {
  SchemaBuilder b;

  auto& single = b.templ("_v");
  single.type = api::FieldDef_::FieldClass::VECTOR;
  single.column = true;
  single.dims = dims;
  single.metric = metric;
  setBenchAnn(single, dims, codec);

  auto& multi = b.templ("_vs");
  multi.type = api::FieldDef_::FieldClass::VECTOR;
  multi.column = true;
  multi.multi = api::FieldDef::MultiMode::DEFAULT;
  multi.dims = dims;
  multi.metric = metric;
  setBenchAnn(multi, dims, codec);

  b.set(col);
}

// The overlay engine/bits a codec arg produces, for index-reuse checks.
bool overlayMatchesCodec(const VectorAuxReader& vaux, int32_t codec) {
  if (codec == PQ || codec == PQ_D8) {
    return vaux.getEngine() == VectorAuxMeta::ENGINE_IVFPQ
      && (codec == PQ_D8) == (vaux.getPqM() == vaux.getDims() / 8);
  }
  return vaux.isRaBitQ() && vaux.getRaBitQBits() == benchRaBitQBits(codec);
}

float nextFloat(SplitMix64& rng) {
  return ((float)((int32_t)rng.rint((int64_t)2001) - 1000)) / 1000.0f;
}

// Forces eligibility so the benchmark measures the aux path. Real runs keep
// the production training floors; unit-test mode shrinks them for tiny corpora.
struct IvfBenchGuard {
  SearchOverridesGuard<int64_t, int64_t, int64_t, int64_t> saved{
    VectorIndexBuilder::ivfPqMinTrainingVectors, VectorIndexBuilder::ivfRaBitQMinTrainingVectors,
    VectorIndexBuilder::ivfBuildThresholdScanCost, VectorIndexBuilder::ivfBuildMinRowsPerTask};

  IvfBenchGuard() {
    VectorIndexBuilder::ivfBuildThresholdScanCost = 0;
    if (luxir::unit_tests) {
      VectorIndexBuilder::ivfPqMinTrainingVectors = 16;
      VectorIndexBuilder::ivfRaBitQMinTrainingVectors = 16;
    }
    // LUXIR_BENCH_IVF_MIN_ROWS overrides the row floor of the build's TBB
    // partitions: a value above any corpus runs row work on the calling
    // thread for A/B comparison. PQ codebooks still train per sub-quantizer.
    if (const char* rows = std::getenv("LUXIR_BENCH_IVF_MIN_ROWS")) {
      VectorIndexBuilder::ivfBuildMinRowsPerTask = std::atoll(rows);
    }
  }
};

std::vector<float> makeVector(int64_t docId, int32_t dims, int32_t valueOrd) {
  SplitMix64 rng((uint64_t)docId * 0x9e3779b97f4a7c15ULL + (uint64_t)(valueOrd + 1) * 0xbf58476d1ce4e5b9ULL);
  std::vector<float> vec((size_t)dims);
  for (int32_t i = 0; i < dims; i++) {
    vec[(size_t)i] = nextFloat(rng);
  }
  return vec;
}

std::vector<float> makeQueryVector(int32_t dims) {
  SplitMix64 rng(0x123456789abcdef0ULL);
  std::vector<float> vec((size_t)dims);
  for (int32_t i = 0; i < dims; i++) {
    vec[(size_t)i] = nextFloat(rng);
  }
  return vec;
}

// The deepen-forcing corpus shape (skew=true): one doc in a thousand is a
// "hog" holding 32 vectors clustered tightly right next to the fixed bench
// query; the rest hold 2 independent random vectors.  Every vector the
// engine ranks near the query then belongs to one of ~50 hog docs, so the
// round-1 pool collapses to far fewer docs than targetDocReq and the widen
// loop must take depth rounds.  (The realistic analog: long chunked
// documents that match the query own all the best chunks.)  The shape is
// deliberately extreme because two softer skews DON'T deepen, which is
// worth remembering: uniform corpora never underfill (avgMult is exact for
// them), and "many mildly-near hogs" doesn't either under IVF+PQ -
// quantization error smears the per-doc clusters across the approximate
// ranking, spreading the top pool over plenty of distinct docs.  Underfill
// requires the near-query vector population itself to be owned by few docs.
// The flat column engine never deepens on multiplicity at all (it
// doc-collapses during its scan), so the skew point is IVF-only.
bool skewHog(int64_t docId) { return docId % 1000 == 0; }

Doc makeVectorDoc(int64_t docId, int32_t dims, int32_t valuesPerDoc, bool skew) {
  std::string id = "d" + std::to_string(docId);
  if (valuesPerDoc == 1 && !skew) {
    return flatdoc("id", id, "bench_v", makeVector(docId, dims, 0));
  }

  std::vector<std::vector<float>> values;
  if (skew && skewHog(docId)) {
    std::vector<float> query = makeQueryVector(dims);
    std::vector<float> center = makeVector(docId, dims, 0);
    for (int32_t i = 0; i < dims; i++) {
      center[(size_t)i] = query[(size_t)i] + 0.02f * center[(size_t)i];
    }
    constexpr int32_t hogValues = 32;
    values.reserve((size_t)hogValues);
    for (int32_t v = 0; v < hogValues; v++) {
      std::vector<float> jitter = makeVector(docId, dims, v + 1);
      for (int32_t i = 0; i < dims; i++) {
        jitter[(size_t)i] = center[(size_t)i] + 0.01f * jitter[(size_t)i];
      }
      values.push_back(std::move(jitter));
    }
  } else {
    int32_t numValues = skew ? 2 : valuesPerDoc;
    values.reserve((size_t)numValues);
    for (int32_t i = 0; i < numValues; i++) {
      values.push_back(makeVector(docId, dims, i));
    }
  }
  return flatdoc("id", id, "bench_vs", values);
}

void indexVectorBatch(CollectionHelper& helper, int64_t startDoc, int64_t count,
                      int32_t dims, int32_t valuesPerDoc, bool skew) {
  constexpr int64_t batchSize = 256;
  std::vector<Doc> docs;
  docs.reserve((size_t)batchSize);

  for (int64_t i = 0; i < count; i++) {
    docs.push_back(makeVectorDoc(startDoc + i, dims, valuesPerDoc, skew));
    if ((int64_t)docs.size() == batchSize) {
      helper.indexAll(docs);
      docs.clear();
    }
  }
  if (!docs.empty()) {
    helper.indexAll(docs);
  }
}

void buildVectorBenchIndex(CollectionHelper& helper, int64_t nDocs, int32_t dims,
                           int32_t valuesPerDoc, bool buildFaissAux, bool skew = false) {
  helper.clear();
  installVectorBenchSchema(helper.collection(), dims);

  int64_t nSegs = luxir::unit_tests ? 2 : 4;
  int64_t baseSegDocs = nDocs / nSegs;
  int64_t remainder = nDocs % nSegs;
  int64_t docId = 0;
  for (int64_t seg = 0; seg < nSegs; seg++) {
    int64_t segDocs = baseSegDocs + (seg < remainder ? 1 : 0);
    indexVectorBatch(helper, docId, segDocs, dims, valuesPerDoc, skew);
    helper.commit();
    docId += segDocs;
  }

  if (buildFaissAux) {
    IvfBenchGuard guard;
    helper.commit({std::string("vec.")
                   + (valuesPerDoc == 1 && !skew ? "bench_v" : "bench_vs")});
  }
}

LocalReq* makeKnnBenchReq(SearchEngine& engine, std::string_view field,
                          const std::vector<float>& queryVec, int32_t k,
                          int32_t nprobe = 0, int32_t refineCandidates = 0,
                          bool exact = false, bool groups = false) {
  auto* req = LocalReq::create(engine);
  req->collection("main").requestId("vector-bench");

  auto& cur = req->topDocs();
  cur.limit(k).countLimit().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, queryVec, k, nprobe, exact, refineCandidates);
  if (groups) {
    std::get<api::KnnQuery>(cur.rawQuery().kind).unit = "groups";
    auto* collapse = new (cur.mr().allocate(sizeof(api::Collapse), alignof(api::Collapse))) api::Collapse();
    api::build::allocArray(collapse->field, 1, cur.mr())[0] = "id";
    std::get<api::TopDocs>(cur.rawOp().kind).collapse = collapse;
  }

  return req;
}

bool fingerprint(LocalReq& req, uint64_t& fp, std::string& error) {
  if (req.responses.empty()) {
    error = "missing search response";
    return false;
  }
  const auto& response = req.responses[0]->proto;
  if (response.error) {
    error = std::string(response.error->message);
    return false;
  }
  const auto* opPtr = response.ops.find("q");
  if (!opPtr || !std::holds_alternative<api::DocList>((*opPtr)->kind)) {
    error = "missing q doc list";
    return false;
  }
  const auto& docs = std::get<api::DocList>((*opPtr)->kind);
  fp = (uint64_t)(docs.found ? *docs.found : 0);
  const auto* idCol = docs.columns.find("id");
  if (!idCol || !std::holds_alternative<api::ColStr>(idCol->kind)) {
    error = "missing id column";
    return false;
  }
  const auto& ids = std::get<api::ColStr>(idCol->kind);
  for (const auto& id : ids.v) {
    fp = fp * 131 + (uint32_t)java_string_hashcode(id);
  }
  return true;
}

void BM_VectorKnn(benchmark::State& state, bool multiValued, bool faissAux,
                  bool skew = false) {
  ForceIvfGuard forceIvf;
  int64_t nDocs = luxir::unit_tests
      ? LuxirTest::scaleTestWork(240)
      : state.range(0);
  int32_t dims = luxir::unit_tests ? 16 : 64;
  int32_t valuesPerDoc = multiValued ? 4 : 1;
  int32_t k = 10;

  CollectionHelper helper("main");
  buildVectorBenchIndex(helper, nDocs, dims, valuesPerDoc, faissAux, skew);
  std::vector<float> query = makeQueryVector(dims);
  std::string field = multiValued ? "bench_vs" : "bench_v";

  uint64_t expectedFp = 0;
  bool haveExpectedFp = false;
  int64_t folds0 = KnnQuery::seenFoldsForTests.load(std::memory_order_relaxed);
  RSSWatcher watcher;
  for (auto _ : state) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), field, query, k);
    req->execute(false);
    std::string error;
    uint64_t fp = 0;
    bool ok = fingerprint(*req, fp, error);
    req->done();

    if (!ok) {
      state.SkipWithError(error.c_str());
      break;
    }
    if (haveExpectedFp && fp != expectedFp) {
      state.SkipWithError("KNN benchmark fingerprint changed");
      break;
    }
    expectedFp = fp;
    haveExpectedFp = true;
    benchmark::DoNotOptimize(fp);
  }

  auto mem = watcher.getDeltaKB();
  state.counters["fp"] = (double)expectedFp;
  state.counters["nDocs"] = (double)nDocs;
  state.counters["dims"] = (double)dims;
  state.counters["values"] = (double)valuesPerDoc;
  state.counters["aux"] = faissAux ? 1.0 : 0.0;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
  // ~1.0 when every query deepened (the skew points exist to pin this at 1;
  // the uniform points pin it at 0 - their collapse never underfills).
  state.counters["folds"] =
    (double)(KnnQuery::seenFoldsForTests.load(std::memory_order_relaxed) - folds0)
    / (double)state.iterations();
}

// Isolate flat traversal and preparation with an already-materialized domain.
// shape: dense single, sparse single (one doc in four), multi (four values).
// arm: 0 = sequential ranks, 1 = domain members. Corpus reuse keeps the
// small unit-test sweep cheap and avoids rebuilding between paired arms.
void BM_VectorKnnFlatDomain(benchmark::State& state) {
  int shape = (int)state.range(0), basisPoints = (int)state.range(1), arm = (int)state.range(2);
  int nDocs = luxir::unit_tests ? 240 : 200'000;
  int dims = luxir::unit_tests ? 8 : 128;
  CollectionHelper helper("flat_domain_" + std::to_string(shape));
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  if (reader->segments().empty()) {
    installVectorBenchSchema(helper.collection(), dims);
    std::vector<Doc> docs;
    for (int i = 0; i < nDocs; i++) {
      auto doc = shape == 1 && i % 4 != 0 ? flatdoc("id", std::to_string(i))
          : makeVectorDoc(i, dims, shape == 2 ? 4 : 1, false);
      docs.push_back(std::move(doc));
      if (docs.size() == 256 || i + 1 == nDocs) { helper.indexAll(docs); docs.clear(); }
    }
    helper.commit();
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
  }
  if (reader->segments().size() != 1) { state.SkipWithError("expected one flat segment"); return; }
  std::string field = shape == 2 ? "bench_vs" : "bench_v";
  auto& seg = reader->segments()[0];
  if (seg.getAuxReader("vec." + field)) { state.SkipWithError("unexpected vector overlay"); return; }
  int withValue = shape == 1 ? nDocs / 4 : nDocs;
  int count = std::max(1, withValue * basisPoints / 10000);
  std::vector<int32_t> members;
  for (int i = 0; i < count; i++) members.push_back((int)((int64_t)i * withValue / count) * (shape == 1 ? 4 : 1));
  ArrDocSet array(std::move(members));
  RAMBitDocSet bitmap(nDocs);
  array.forEach([&](int doc) { bitmap.mutableBits().set(doc); });
  // Match the normal array/bitset storage crossover, including a present
  // full domain so the 100% point still compares both filtered arms.
  DocSet* domain = count * (int)sizeof(int32_t) < nDocs / 8 ? (DocSet*)&array : &bitmap;
  const auto& type = (const VectorFieldType&)*reader->schema()->getFieldTypePtr(field);
  auto vector = makeQueryVector(dims);
  SearchOverridesGuard saved(KnnQuery::flatDomainDensityThreshold, KnnQuery::statsForTests);
  auto run = [&]() {
    MemPool pool;
    Query::Context context(pool, *reader);
    KnnQuery query(field, type, vector, 10);
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    std::array<DocSet*, 1> domains{domain};
    Query::Weight::PrepareContext prepare(*reader, domains, false, nullptr);
    auto prepared = weight->prepare(prepare);
    auto* scorer = prepared->createScorer(pool, seg);
    uint64_t fp = 0;
    if (scorer) for (int doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next())
      fp = fp * 131 + (uint64_t)doc;
    return fp;
  };
  KnnQuery::flatDomainDensityThreshold = -1;
  uint64_t expected = run();
  KnnQuery::flatDomainDensityThreshold = INFINITY;
  if (run() != expected) { state.SkipWithError("flat traversal arms disagree"); return; }
  KnnQuery::flatDomainDensityThreshold = arm ? INFINITY : -1;
  KnnQuery::Stats stats;
  KnnQuery::statsForTests = &stats;
  run();
  KnnQuery::statsForTests = nullptr;
  for (auto _ : state) {
    uint64_t fp = run();
    benchmark::DoNotOptimize(fp);
    if (fp != expected) { state.SkipWithError("flat traversal fingerprint changed"); break; }
  }
  state.counters["domain_density"] = (double)count / withValue;
  state.counters["domain_docs"] = (double)stats.domainDocsVisited.load();
  state.counters["domain_probes"] = (double)stats.domainProbes.load();
  state.counters["vectors_scored"] = (double)stats.vectorsScored.load();
  state.counters["nDocs"] = nDocs;
  state.counters["dims"] = dims;
}

// ---------------------------------------------------------------------------
// IVF+PQ recall benchmark.
//
// Measures recall@k of the production IVF+PQ defaults against ground truth
// from the exact contract path (KnnQuery.exact), through the same request
// pipeline - same filters, collapse, and score scale, so the only variable is
// the index. The nprobe arg sweeps breadth (0 = the query's adaptive default,
// large = exhaustive over lists); recall and latency move together, which is
// the curve that decides the shipping default.
//
// Data is clustered, not uniform: real embedding corpora cluster, and uniform
// random vectors are IVF's pathological worst case - recall measured on noise
// would slander any default.

// Cluster center for clusterId: deterministic, components in [-1, 1].
std::vector<float> clusterCenter(int32_t clusterId, int32_t dims) {
  SplitMix64 rng((uint64_t)(clusterId + 1) * 0x2545f4914f6cdd1dULL);
  std::vector<float> center((size_t)dims);
  for (int32_t i = 0; i < dims; i++) {
    center[(size_t)i] = nextFloat(rng);
  }
  return center;
}

// Doc vector: its cluster's center plus +-0.15 noise.  Deterministic per doc.
std::vector<float> makeClusteredVector(int64_t docId, int32_t dims, int32_t nClusters) {
  int32_t cluster = (int32_t)(docId % nClusters);
  std::vector<float> vec = clusterCenter(cluster, dims);
  SplitMix64 rng((uint64_t)docId * 0x9e3779b97f4a7c15ULL + 0x6a09e667f3bcc909ULL);
  for (int32_t i = 0; i < dims; i++) {
    vec[(size_t)i] += 0.15f * nextFloat(rng);
  }
  return vec;
}

// In-distribution query: a cluster center plus noise, distinct seed stream
// from any doc.
std::vector<float> makeClusteredQuery(int32_t queryOrd, int32_t dims, int32_t nClusters) {
  std::vector<float> vec = clusterCenter(queryOrd % nClusters, dims);
  SplitMix64 rng((uint64_t)(queryOrd + 1) * 0xd6e8feb86659fd93ULL);
  for (int32_t i = 0; i < dims; i++) {
    vec[(size_t)i] += 0.15f * nextFloat(rng);
  }
  return vec;
}

std::vector<int32_t> clusteredDocsPerSeg(int64_t nDocs, int64_t nSegs = 4) {
  if (luxir::unit_tests) nSegs = 2;
  std::vector<int32_t> docsPerSeg((size_t)nSegs);
  for (int64_t seg = 0; seg < nSegs; seg++) {
    docsPerSeg[(size_t)seg] = (int32_t)(nDocs / nSegs + (seg < nDocs % nSegs ? 1 : 0));
  }
  return docsPerSeg;
}

// Reuse check (same pattern as QueryBM / FacetBM): segment shape must match,
// but shape alone is ambiguous here - BM_VectorKnn builds same-sized indexes
// with uniform IP data - so also verify the aux is our clustered L2 build of
// the requested codec via its metadata.
bool clusteredIndexReusable(CollectionHelper& helper, std::span<const int32_t> docsPerSeg,
                            int32_t codec) {
  if (!helper.indexMatchesShape(docsPerSeg)) return false;
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  for (const auto& seg : reader->segments()) {
    auto aux = seg.getAuxReader("vec.bench_v");
    if (!aux) return false;
    auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
    if (vaux == nullptr || !overlayMatchesCodec(*vaux, codec) ||
        vaux->getMetric() != (int32_t)api::VectorMetric::L2) {
      return false;
    }
  }
  return true;
}

void buildClusteredVectorIndex(CollectionHelper& helper, int32_t dims, int32_t nClusters,
                               std::span<const int32_t> docsPerSeg, int32_t codec) {
  helper.clear();
  installVectorBenchSchema(helper.collection(), dims, api::VectorMetric::L2, codec);

  constexpr int64_t batchSize = 256;
  std::vector<Doc> docs;
  docs.reserve((size_t)batchSize);
  int64_t docId = 0;
  for (int32_t segDocs : docsPerSeg) {
    for (int32_t i = 0; i < segDocs; i++, docId++) {
      docs.push_back(flatdoc("id", "d" + std::to_string(docId),
                             "bench_v", makeClusteredVector(docId, dims, nClusters)));
      if ((int64_t)docs.size() == batchSize) {
        helper.indexAll(docs);
        docs.clear();
      }
    }
    if (!docs.empty()) {
      helper.indexAll(docs);
      docs.clear();
    }
    helper.commit();
  }
  helper.commit({"vec.bench_v"});
}

bool responseIds(LocalReq& req, std::vector<std::string>& out, std::string& error) {
  if (req.responses.empty()) {
    error = "missing search response";
    return false;
  }
  const auto& response = req.responses[0]->proto;
  if (response.error) {
    error = std::string(response.error->message);
    return false;
  }
  const auto* opPtr = response.ops.find("q");
  if (!opPtr) {
    error = "missing q response op";
    return false;
  }
  if (!std::holds_alternative<api::DocList>((*opPtr)->kind)) {
    error = "q response op is not a doc list";
    return false;
  }
  const auto& docs = std::get<api::DocList>((*opPtr)->kind);
  const auto* idCol = docs.columns.find("id");
  if (!idCol) {
    error = "missing id column";
    return false;
  }
  if (!std::holds_alternative<api::ColStr>(idCol->kind)) {
    error = "id column is not string";
    return false;
  }
  const auto& ids = std::get<api::ColStr>(idCol->kind);
  out.assign(ids.v.begin(), ids.v.end());
  return true;
}

void forceIvfBenchmark(benchmark::Benchmark* bench) {
  static std::optional<ForceIvfGuard> routing;
  bench->Setup([](const benchmark::State&) { routing.emplace(); });
  bench->Teardown([](const benchmark::State&) { routing.reset(); });
}

void BM_VectorKnnRecallBody(benchmark::State& state, bool parallelExec,
                            bool exact = false, bool groups = false) {
  if (luxir::unit_tests && state.threads() > 4) {
    state.SkipWithMessage("reduced vector unit-test thread sweep");
    return;
  }
  int64_t nDocs = luxir::unit_tests
      ? LuxirTest::scaleTestDimension(240, 2)
      : 50'000;
  int32_t dims = luxir::unit_tests ? 16 : 64;
  int32_t nClusters = luxir::unit_tests ? 8 : 100;
  int32_t nQueries = luxir::unit_tests
      ? (int32_t)LuxirTest::scaleTestDimension(4, 2)
      : 16;
  int32_t nprobe = (int32_t)state.range(0);
  int32_t cand = (int32_t)state.range(1);  // candidate pool size; 0 = adaptive default
  int32_t k = (int32_t)state.range(2);
  int32_t codec = (int32_t)state.range(3);

  CollectionHelper helper("main");
  std::vector<int32_t> docsPerSeg = clusteredDocsPerSeg(nDocs);

  // Setup must be concurrency-safe: with ->Threads(N), every thread runs this
  // function body.  One thread builds (or confirms reuse of) the index and
  // the per-k ground truth under a mutex; the rest wait, then read.  Ground
  // truth is cached across benchmark instances per k - the index content is
  // fixed for the process lifetime.
  static std::mutex setupMutex;
  static std::map<int32_t, std::vector<std::vector<std::string>>> truthByK;
  bool reuseIndex;
  std::vector<std::vector<float>> queries;
  for (int32_t q = 0; q < nQueries; q++) {
    queries.push_back(makeClusteredQuery(q, dims, nClusters));
  }
  std::vector<std::vector<std::string>> truth;
  {
    std::lock_guard<std::mutex> lock(setupMutex);
    reuseIndex = clusteredIndexReusable(helper, docsPerSeg, codec);
    if (!reuseIndex) {
      IvfBenchGuard guard;
      buildClusteredVectorIndex(helper, dims, nClusters, docsPerSeg, codec);
    }
    auto it = truthByK.find(k);
    if (it == truthByK.end()) {
      std::vector<std::vector<std::string>> kTruth;
      for (int32_t q = 0; q < nQueries; q++) {
        auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v", queries[(size_t)q],
                                    k, /*nprobe=*/0, /*refineCandidates=*/0, /*exact=*/true);
        req->execute(false);
        std::vector<std::string> ids;
        std::string error;
        bool ok = responseIds(*req, ids, error);
        req->done();
        if (!ok) {
          state.SkipWithError(error.c_str());
          return;
        }
        kTruth.push_back(std::move(ids));
      }
      it = truthByK.emplace(k, std::move(kTruth)).first;
    }
    truth = it->second;
  }

  // Recall is measured deterministically OUTSIDE the timed loop: exactly one
  // ANN run per query, mean over all queries.  Averaging inside the loop
  // overweights early-rotation queries whenever the iteration count is not a
  // multiple of nQueries (~+-0.01 wobble observed).
  KnnQuery::Stats stats;
  std::optional<SearchOverridesGuard<KnnQuery::Stats*>> statsGuard;
  if (state.threads() == 1) {
    statsGuard.emplace(KnnQuery::statsForTests);
    KnnQuery::statsForTests = &stats;
  }
  double recallSum = 0;
  for (int32_t q = 0; q < nQueries; q++) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v",
                                queries[(size_t)q], k, nprobe, cand, exact, groups);
    req->execute(false);
    std::vector<std::string> ids;
    std::string error;
    bool ok = responseIds(*req, ids, error);
    req->done();
    if (!ok) {
      state.SkipWithError(error.c_str());
      return;
    }
    const auto& expected = truth[(size_t)q];
    int32_t hits = 0;
    for (const auto& id : ids) {
      if (std::find(expected.begin(), expected.end(), id) != expected.end()) hits++;
    }
    recallSum += (double)hits / (double)expected.size();
  }

  statsGuard.reset();

  // Timed loop measures latency only, cycling queries for realistic variety.
  // parallelExec runs each request on the TBB task group (intra-query scan /
  // rescore tasks); results are identical either way, only latency moves.
  int32_t queryOrd = 0;
  for (auto _ : state) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v",
                                queries[(size_t)queryOrd], k, nprobe, cand, exact, groups);
    req->execute(parallelExec);
    std::vector<std::string> ids;
    std::string error;
    bool ok = responseIds(*req, ids, error);
    req->done();
    if (!ok) {
      state.SkipWithError(error.c_str());
      break;
    }
    queryOrd = (queryOrd + 1) % nQueries;
    benchmark::DoNotOptimize(ids);
  }

  // Config/recall counters are identical across threads - average them so
  // threaded runs report the same readable values as single-threaded runs.
  // rate sums iterations across threads over wall time = aggregate QPS.
  using benchmark::Counter;
  state.counters["recall"] = Counter(recallSum / (double)nQueries, Counter::kAvgThreads);
  state.counters["nprobe"] = Counter((double)nprobe, Counter::kAvgThreads);
  state.counters["cand"] = Counter((double)cand, Counter::kAvgThreads);
  state.counters["lists"] = (double)stats.lists.load() / nQueries;
  state.counters["vectors"] = (double)stats.vectors.load() / nQueries;
  state.counters["scanned"] = (double)stats.scanned.load() / nQueries;
  state.counters["extra_bits"] = (double)stats.extraBitEvaluations.load() / nQueries;
  state.counters["gate_rejections"] = (double)stats.gateRejections.load() / nQueries;
  state.counters["retained_peak"] = (double)stats.retainedPeak.load() / nQueries;
  state.counters["retained_terminal"] = (double)stats.retainedTerminal.load() / nQueries;
  state.counters["seed_reads"] = (double)stats.seedReads.load() / nQueries;
  state.counters["remaining_reads"] = (double)stats.remainingReads.load() / nQueries;
  state.counters["bytes_read"] = (double)stats.bytes.load() / nQueries;
  state.counters["rounds"] = (double)stats.rounds.load() / nQueries;
  state.counters["exact"] = exact;
  state.counters["groups"] = groups;
  state.counters["reused"] = Counter(reuseIndex ? 1.0 : 0.0, Counter::kAvgThreads);
  state.counters["k"] = Counter((double)k, Counter::kAvgThreads);
  state.counters["codec"] = Counter((double)codec, Counter::kAvgThreads);
  state.SetLabel(benchCodecName(codec));
  state.counters["nDocs"] = Counter((double)nDocs, Counter::kAvgThreads);
  state.counters["dims"] = Counter((double)dims, Counter::kAvgThreads);
  state.counters["rate"] = Counter(state.iterations(), Counter::kIsRate);
}

void BM_VectorKnnRecall(benchmark::State& state) {
  BM_VectorKnnRecallBody(state, /*parallelExec=*/false);
}

// Single-client latency with intra-query parallelism: the (segment,
// list-range) scan tasks and per-segment rescore fan out across idle cores.
// Compare against BM_VectorKnnRecall (serial execution of the same work) for
// the latency win; recall is identical by construction.
void BM_VectorKnnRecallParallel(benchmark::State& state) {
  BM_VectorKnnRecallBody(state, /*parallelExec=*/true);
}

// Throughput wrapper: same benchmark at the production defaults, run on
// 1..32 concurrent closed-loop threads.  rate = aggregate QPS.  Registered
// as a separate function so the thread sweep does not multiply the full
// (nprobe, refine, k) grid.
void BM_VectorKnnRecallThreads(benchmark::State& state) {
  BM_VectorKnnRecallBody(state, /*parallelExec=*/false);
}

// Saturated-throughput check for intra-query parallelism: under closed-loop
// load every core is already busy, so task fan-out cannot ADD throughput -
// this run exists to confirm the scheduling overhead does not SUBTRACT
// from it relative to BM_VectorKnnRecallThreads.
void BM_VectorKnnRecallThreadsParallel(benchmark::State& state) {
  BM_VectorKnnRecallBody(state, /*parallelExec=*/true);
}

// Build-phase cost of the IVF aux index for the codec arg.  Manual timing
// covers ONLY the aux commit (train + encode + serialize) - the marginal cost
// of the ANN index over plain column storage.  Doc indexing wall time is
// reported as a counter for context.  Fixed iteration count: each iteration
// is a full rebuild, so let it be expensive but bounded.
void BM_VectorIvfBuild(benchmark::State& state) {
  int64_t nDocs = luxir::unit_tests
      ? LuxirTest::scaleTestWork(240)
      : state.range(1);
  int32_t dims = luxir::unit_tests ? 16 : (int32_t)state.range(2);
  int32_t nClusters = luxir::unit_tests ? 8 : 100;
  int32_t codec = (int32_t)state.range(0);

  IvfBenchGuard guard;
  CollectionHelper helper("main");
  std::vector<int32_t> docsPerSeg = clusteredDocsPerSeg(nDocs, state.range(3));

  double indexDocsSecs = 0;
  for (auto _ : state) {
    // Untimed (for the manual clock): clear, schema, index docs, segment
    // commits.  buildClusteredVectorIndex's final aux commit is what we
    // want isolated, so inline its steps here.
    auto wallStart = std::chrono::steady_clock::now();
    helper.clear();
    installVectorBenchSchema(helper.collection(), dims, api::VectorMetric::L2, codec);
    constexpr int64_t batchSize = 256;
    std::vector<Doc> docs;
    docs.reserve((size_t)batchSize);
    int64_t docId = 0;
    for (int32_t segDocs : docsPerSeg) {
      for (int32_t i = 0; i < segDocs; i++, docId++) {
        docs.push_back(flatdoc("id", "d" + std::to_string(docId),
                               "bench_v", makeClusteredVector(docId, dims, nClusters)));
        if ((int64_t)docs.size() == batchSize) {
          helper.indexAll(docs);
          docs.clear();
        }
      }
      if (!docs.empty()) {
        helper.indexAll(docs);
        docs.clear();
      }
      helper.commit();
    }
    indexDocsSecs += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wallStart).count();

    // Timed: the aux build commit only.
    BenchTimer timer(state);
    helper.commit({"vec.bench_v"});
  }

  state.counters["nDocs"] = (double)nDocs;
  state.counters["dims"] = (double)dims;
  state.counters["segments"] = (double)docsPerSeg.size();
  state.counters["chunkBytes"] = (double)VectorIndexBuilder::ivfAddChunkBytes;
  state.counters["docIndexSecs"] =
      state.iterations() > 0 ? indexDocsSecs / (double)state.iterations() : 0.0;
  state.counters["vecPerSec"] = benchmark::Counter(
      (double)nDocs * (double)state.iterations(), benchmark::Counter::kIsRate);
}

void BM_VectorIvfIncrementalBuild(benchmark::State& state) {
  bool aboveThresholdFlush = state.range(0) != 0;
  int32_t dims = luxir::unit_tests ? 16 : 64;
  int32_t nClusters = luxir::unit_tests ? 8 : 100;
  int64_t flushDocs = aboveThresholdFlush
    ? (luxir::unit_tests ? 120 : 25'000)
    : (luxir::unit_tests ? 4 : 128);
  int64_t threshold = luxir::unit_tests
    ? (int64_t)dims * 64
    : 1'000'000;

  IvfBenchGuard guard;
  VectorIndexBuilder::ivfBuildThresholdScanCost = threshold;
  CollectionHelper helper("main");
  std::vector<int32_t> docsPerSeg = luxir::unit_tests
    ? std::vector<int32_t>{120, 120}
    : std::vector<int32_t>{25'000, 25'000};

  double setupSecs = 0;
  int64_t annBuilds = 0;
  for (auto _ : state) {
    auto setupStart = std::chrono::steady_clock::now();
    helper.clear();
    installVectorBenchSchema(helper.collection(), dims, api::VectorMetric::L2);
    constexpr int64_t batchSize = 256;
    std::vector<Doc> docs;
    docs.reserve((size_t)batchSize);
    int64_t docId = 0;
    for (int32_t segDocs : docsPerSeg) {
      for (int32_t i = 0; i < segDocs; i++, docId++) {
        docs.push_back(flatdoc("id", "d" + std::to_string(docId),
                               "bench_v", makeClusteredVector(docId, dims, nClusters)));
        if ((int64_t)docs.size() == batchSize) {
          helper.indexAll(docs);
          docs.clear();
        }
      }
      if (!docs.empty()) {
        helper.indexAll(docs);
        docs.clear();
      }
      helper.commit();
    }
    helper.commit({"vec.bench_v"});
    setupSecs += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - setupStart).count();

    for (int64_t i = 0; i < flushDocs; i++, docId++) {
      docs.push_back(flatdoc("id", "d" + std::to_string(docId),
                             "bench_v", makeClusteredVector(docId, dims, nClusters)));
      if ((int64_t)docs.size() == batchSize) {
        helper.indexAll(docs);
        docs.clear();
      }
    }
    if (!docs.empty()) {
      helper.indexAll(docs);
      docs.clear();
    }

    VectorIndexBuilder::ivfBuildCountForTests.store(0, std::memory_order_relaxed);
    VectorIndexBuilder::ivfMergeBuildCountForTests.store(0, std::memory_order_relaxed);
    BenchTimer timer(state);
    helper.commit({"vec.bench_v"});
    annBuilds += VectorIndexBuilder::ivfBuildCountForTests.load(std::memory_order_relaxed);
  }

  state.counters["flushDocs"] = (double)flushDocs;
  state.counters["aboveThreshold"] = aboveThresholdFlush ? 1.0 : 0.0;
  state.counters["annBuilds"] =
      state.iterations() > 0 ? (double)annBuilds / (double)state.iterations() : 0.0;
  state.counters["setupSecs"] =
      state.iterations() > 0 ? setupSecs / (double)state.iterations() : 0.0;
}

// ---------------------------------------------------------------------------
// Codec comparison on a real embedding corpus.
//
// LUXIR_VECTOR_DATASET names a raw little-endian float32 file of rows
// (LUXIR_VECTOR_DIMS, default 768); the last LUXIR_VECTOR_QUERIES rows
// (default 200) are held-out queries and up to LUXIR_VECTOR_N (default: the
// rest) leading rows are indexed into LUXIR_VECTOR_SEGMENTS (default 4)
// segments under LUXIR_VECTOR_METRIC (l2, ip, cosine; default cosine).  Per
// codec it reports the aux build time (train + encode + write over every
// segment), overlay bytes per vector, recall@10 against the exact path, and
// serial single-query latency.  Instances are grouped by codec so the built
// index is reused across (nprobe, cand) points.
struct VectorDataset {
  int32_t dims = 0;
  int64_t nBase = 0;
  int32_t nQueries = 0;
  api::VectorMetric metric = api::VectorMetric::COSINE;
  int32_t segments = 4;
  std::vector<float> data;  // nBase rows, then nQueries rows

  static const VectorDataset* get() {
    static std::optional<VectorDataset> loaded = []() -> std::optional<VectorDataset> {
      const char* path = getenv("LUXIR_VECTOR_DATASET");
      if (path == nullptr) return std::nullopt;
      auto envInt = [](const char* name, int64_t dflt) {
        const char* v = getenv(name);
        return v ? std::stoll(v) : dflt;
      };
      VectorDataset ds;
      ds.dims = (int32_t)envInt("LUXIR_VECTOR_DIMS", 768);
      ds.nQueries = (int32_t)envInt("LUXIR_VECTOR_QUERIES", 200);
      ds.segments = (int32_t)envInt("LUXIR_VECTOR_SEGMENTS", 4);
      std::string metric = getenv("LUXIR_VECTOR_METRIC") ? getenv("LUXIR_VECTOR_METRIC") : "cosine";
      ds.metric = metric == "l2" ? api::VectorMetric::L2
                : metric == "ip" ? api::VectorMetric::IP : api::VectorMetric::COSINE;
      FILE* f = fopen(path, "rb");
      if (f == nullptr) return std::nullopt;
      fseek(f, 0, SEEK_END);
      int64_t rows = (int64_t)ftell(f) / (int64_t)(ds.dims * sizeof(float));
      fseek(f, 0, SEEK_SET);
      ds.nBase = std::min<int64_t>(envInt("LUXIR_VECTOR_N", rows - ds.nQueries), rows - ds.nQueries);
      ds.data.resize((size_t)rows * (size_t)ds.dims);
      size_t got = fread(ds.data.data(), sizeof(float) * (size_t)ds.dims, (size_t)rows, f);
      fclose(f);
      if ((int64_t)got != rows) return std::nullopt;
      // Keep the base prefix and the trailing query rows contiguous.
      std::copy(ds.data.end() - (ptrdiff_t)ds.nQueries * ds.dims, ds.data.end(),
                ds.data.begin() + (ptrdiff_t)ds.nBase * ds.dims);
      ds.data.resize((size_t)(ds.nBase + ds.nQueries) * (size_t)ds.dims);
      return ds;
    }();
    return loaded ? &*loaded : nullptr;
  }

  std::vector<float> row(int64_t i) const {
    return {data.begin() + (ptrdiff_t)i * dims, data.begin() + (ptrdiff_t)(i + 1) * dims};
  }
  std::vector<float> query(int32_t q) const { return row(nBase + q); }
};

// Fixed query anchors have exact distance-rank fields. Near/far range
// filters therefore have real vector-space correlation and exact cardinality;
// a shuffled rank field supplies the independent control. Rank fields and
// exact filtered answers are shared across codecs and alpha settings.
struct FilteredVectorCorpus {
  const VectorDataset* dataset = nullptr;
  int64_t size;
  int dims, segments;
  api::VectorMetric metric;
  std::vector<float> synthetic;
  std::vector<std::vector<float>> queries;
  std::vector<std::vector<int32_t>> nearRanks;
  std::vector<int32_t> randomRank;
  std::map<std::pair<int, int>, std::vector<std::vector<std::string>>> truth;

  std::span<const float> row(int64_t i) const {
    const auto& data = dataset ? dataset->data : synthetic;
    return {data.data() + i * dims, (size_t)dims};
  }

  FilteredVectorCorpus() {
    auto env = [](const char* key, int fallback) { const char* v = getenv(key); return v ? std::stoi(v) : fallback; };
    if (!luxir::unit_tests) dataset = VectorDataset::get();
    if (!luxir::unit_tests && getenv("LUXIR_VECTOR_DATASET") && !dataset)
      throw std::runtime_error("cannot read LUXIR_VECTOR_DATASET");
    size = luxir::unit_tests ? 512 : dataset ? dataset->nBase : env("LUXIR_VECTOR_N", 500'000);
    dims = luxir::unit_tests ? 16 : dataset ? dataset->dims : env("LUXIR_VECTOR_DIMS", 128);
    segments = luxir::unit_tests ? 1 : dataset ? dataset->segments : env("LUXIR_VECTOR_SEGMENTS", 1);
    metric = dataset ? dataset->metric : api::VectorMetric::L2;
    // Each anchor adds a rank column and 4*N bytes of setup memory. Bound
    // that cost at 64 columns (1.28 GB of ranks for a 5M-vector corpus).
    int nq = luxir::unit_tests ? 2 : std::min(64, env("LUXIR_FILTERED_QUERIES", 8));
    if (dataset) nq = std::min(nq, dataset->nQueries);
    if (size <= 0 || size > INT32_MAX || dims <= 0 || nq <= 0 || segments <= 0)
      throw std::runtime_error("invalid filtered vector corpus shape");
    int clusters = luxir::unit_tests ? 8 : 100;
    if (!dataset) {
      synthetic.reserve((size_t)size * dims);
      for (int64_t i = 0; i < size; i++) {
        auto v = makeClusteredVector(i, dims, clusters);
        synthetic.insert(synthetic.end(), v.begin(), v.end());
      }
    }
    randomRank.resize((size_t)size);
    std::vector<int32_t> order((size_t)size);
    for (int32_t i = 0; i < size; i++) order[(size_t)i] = i;
    SplitMix64 rng(0x2468ace);
    for (int64_t i = size - 1; i > 0; i--) std::swap(order[(size_t)i], order[(size_t)rng.rint(i + 1)]);
    for (int32_t i = 0; i < size; i++) randomRank[(size_t)order[(size_t)i]] = i;
    for (int q = 0; q < nq; q++) {
      queries.push_back(dataset ? dataset->query(q) : makeClusteredQuery(q, dims, clusters));
      const auto& query = queries.back();
      float qnorm = faiss::fvec_inner_product(query.data(), query.data(), dims);
      std::vector<std::pair<float, int32_t>> ranked;
      ranked.reserve((size_t)size);
      for (int32_t i = 0; i < size; i++) {
        auto v = row(i);
        float score = metric == api::VectorMetric::L2 ? -faiss::fvec_L2sqr(query.data(), v.data(), dims)
            : faiss::fvec_inner_product(query.data(), v.data(), dims);
        if (metric == api::VectorMetric::COSINE) {
          float norm = qnorm * faiss::fvec_inner_product(v.data(), v.data(), dims);
          score = norm > 0 ? score / std::sqrt(norm) : 0;
        }
        ranked.emplace_back(score, i);
      }
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
      });
      auto& ranks = nearRanks.emplace_back((size_t)size);
      for (int32_t i = 0; i < size; i++) ranks[(size_t)ranked[(size_t)i].second] = i;
    }
  }
};

void BM_VectorKnnFiltered(benchmark::State& state) {
  static std::optional<FilteredVectorCorpus> corpus;
  if (!corpus) corpus.emplace();
  auto& data = *corpus;
  int codec = (int)state.range(0), permyriad = (int)state.range(1), correlation = (int)state.range(2);
  int64_t selected = std::max((int64_t)1, data.size * permyriad / 10000);
  CollectionHelper helper("filtered_vectors");
  static int builtCodec = -1;
  if (builtCodec != codec) {
    helper.clear();
    installVectorBenchSchema(helper.collection(), data.dims, data.metric, codec);
    std::vector<Doc> docs;
    int64_t perSegment = (data.size + data.segments - 1) / data.segments;
    for (int64_t i = 0; i < data.size; i++) {
      auto v = data.row(i);
      auto doc = flatdoc("id", "d" + std::to_string(i), "bench_v", std::vector<float>(v.begin(), v.end()),
                        "random_i", (int64_t)data.randomRank[(size_t)i]);
      for (size_t q = 0; q < data.queries.size(); q++)
        doc.emplace_back("near" + std::to_string(q) + "_i", (int64_t)data.nearRanks[q][(size_t)i]);
      docs.push_back(std::move(doc));
      bool commit = (i + 1) % perSegment == 0 || i + 1 == data.size;
      if (docs.size() == 256 || commit) { helper.indexAll(docs); docs.clear(); }
      if (commit) helper.commit();
    }
    IvfBenchGuard guard;
    helper.commit({"vec.bench_v"});
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    for (const auto& seg : reader->segments()) {
      if (!seg.getAuxReader("vec.bench_v")) {
        state.SkipWithError("filtered benchmark segment has no IVF overlay; increase corpus size");
        return;
      }
    }
    builtCodec = codec;
  }
  SearchOverridesGuard saved(KnnQuery::filteredCoverageAlpha, KnnQuery::statsForTests, KnnQuery::routing);
  KnnQuery::routing = (KnnQuery::Routing)state.range(4);
  KnnQuery::filteredCoverageAlpha = (double)state.range(3) / 100;
  auto run = [&](int q, bool exact, std::vector<std::string>& ids) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("filtered_vectors").topDocs();
    cur.limit(10).countLimit().fields({"id"});
    cur.rawQuery() = qb::knn(cur.mr(), "bench_v", data.queries[(size_t)q], 10, 0, exact);
    std::string field = correlation == 0 ? "random_i" : "near" + std::to_string(q) + "_i";
    int64_t begin = correlation == 2 ? data.size - selected : correlation == 3 ? (data.size - selected) / 2 : 0;
    cur.filter(qb::range(cur.mr(), field, qb::valI64(cur.mr(), begin), nullptr,
                        nullptr, qb::valI64(cur.mr(), begin + selected)));
    req->execute(false);
    std::string error;
    if (!responseIds(*req, ids, error)) { state.SkipWithError(error.c_str()); return false; }
    return true;
  };
  auto key = std::pair{permyriad, correlation};
  auto it = data.truth.find(key);
  if (it == data.truth.end()) {
    std::vector<std::vector<std::string>> truth(data.queries.size());
    for (int q = 0; q < (int)data.queries.size(); q++) if (!run(q, true, truth[(size_t)q])) return;
    it = data.truth.emplace(key, std::move(truth)).first;
  }
  KnnQuery::statsForTests = nullptr;
  auto exactStart = std::chrono::steady_clock::now();
  for (int q = 0; q < (int)data.queries.size(); q++) {
    std::vector<std::string> ids;
    if (!run(q, true, ids)) return;
  }
  double exactMicros = std::chrono::duration<double, std::micro>(
      std::chrono::steady_clock::now() - exactStart).count() / data.queries.size();
  KnnQuery::Stats stats;
  KnnQuery::statsForTests = &stats;
  double recall = 0;
  for (int q = 0; q < (int)data.queries.size(); q++) {
    std::vector<std::string> ids;
    if (!run(q, false, ids)) return;
    const auto& expected = it->second[(size_t)q];
    int hits = 0;
    for (const auto& id : ids) hits += std::ranges::find(expected, id) != expected.end();
    recall += expected.empty() ? 1.0 : (double)hits / expected.size();
  }
  KnnQuery::statsForTests = nullptr;
  int q = 0;
  for (auto _ : state) {
    std::vector<std::string> ids;
    if (!run(q, false, ids)) break;
    benchmark::DoNotOptimize(ids);
    q = (q + 1) % (int)data.queries.size();
  }
  double queries = (double)data.queries.size();
  state.SetLabel(std::string(benchCodecName(codec)) + "/" + (correlation == 0 ? "independent" : correlation == 1 ? "near" : correlation == 2 ? "far" : "median"));
  state.counters["planned_exact"] = stats.plannedExact.load() / queries;
  state.counters["planned_ivf"] = stats.plannedIvf.load() / queries;
  state.counters["request_cost"] = KnnQuery::plannerCosts.requestCost;
  state.counters["predicted_cost"] = stats.predictedCost.load() / queries;
  state.counters["actual_cost"] = stats.modeledActualCost.load() / queries;
  for (size_t i = 0; i < KnnPlanner::NUM_UNITS; i++)
    state.counters["unit_" + std::string(KnnPlanner::unitNames[i])] = stats.costWork[i].load() / queries;
  state.counters["work_listSetups"] = stats.work.listSetups / queries;
  state.counters["work_physicalEntries"] = stats.work.physicalEntries / queries;
  state.counters["work_selectorPasses"] = stats.work.selectorPasses / queries;
  state.counters["work_newListSelectorPasses"] = stats.work.newListSelectorPasses / queries;
  state.counters["work_newListLiveVectors"] = stats.work.newListLiveVectors / queries;
  state.counters["work_depthEntries"] = stats.work.depthEntries / queries;
  state.counters["work_pqScores"] = stats.work.pqScores / queries;
  state.counters["work_oneBitScores"] = stats.work.oneBitScores / queries;
  state.counters["work_extraBitScores"] = stats.work.extraBitScores / queries;
  state.counters["work_gateRejections"] = stats.work.gateRejections / queries;
  state.counters["work_exactVectors"] = stats.work.exactVectors / queries;
  state.counters["work_domainDocs"] = stats.work.domainDocs / queries;
  state.counters["work_domainProbes"] = stats.work.domainProbes / queries;
  state.counters["work_seedReads"] = stats.work.seedReads / queries;
  state.counters["work_remainingReads"] = stats.work.remainingReads / queries;
  state.counters["work_scoreBytes"] = stats.work.scoreBytes / queries;
  state.counters["work_rankVisits"] = stats.work.rankVisits / queries;
  state.counters["work_sparseMaps"] = stats.work.sparseMaps / queries;
  state.counters["work_multiMaps"] = stats.work.multiMaps / queries;
  state.counters["work_coarseDims"] = stats.work.coarseDims / queries;
  state.counters["work_ivfRounds"] = stats.work.ivfRounds / queries;
  state.counters["work_rounds"] = stats.work.rounds / queries;
  state.counters["work_selectorNone"] = stats.work.selectorNone / queries;
  state.counters["work_selectorBitmap"] = stats.work.selectorBitmap / queries;
  state.counters["work_resolveIdentity"] = stats.work.resolveIdentity / queries;
  state.counters["work_resolveSelect"] = stats.work.resolveSelect / queries;
  state.counters["work_resolveMono"] = stats.work.resolveMono / queries;
  state.counters["work_bitProbes"] = stats.work.bitProbes / queries;
  state.counters["work_allocatorEntries"] = stats.work.allocatorEntries / queries;
  state.counters["work_arraySteps"] = stats.work.arraySteps / queries;
  state.counters["exact_us"] = exactMicros;
  state.counters["recall@10"] = recall / queries;
  state.counters["recall_pass"] = recall / queries >= 0.95;
  state.counters["rounds"] = stats.rounds.load() / queries;
  state.counters["codes_visited"] = stats.codesVisited.load() / queries;
  state.counters["selector_passes"] = stats.scanned.load() / queries;
  state.counters["pq_scores"] = stats.pqScores.load() / queries;
  state.counters["one_bit_scores"] = stats.oneBitScores.load() / queries;
  state.counters["extra_bit_scores"] = stats.extraBitEvaluations.load() / queries;
  state.counters["new_list_passes"] = stats.newListSelectorPasses.load() / queries;
  state.counters["depth_entries"] = stats.depthEntries.load() / queries;
  state.counters["list_setups"] = stats.lists.load() / queries;
  state.counters["flat_scores"] = stats.vectorsScored.load() / queries;
  state.counters["domain_docs"] = stats.domainDocsVisited.load() / queries;
  state.counters["terminal_reads"] = stats.terminalReads.load() / queries;
  state.counters["coverage"] = stats.coveredFraction.load() / queries;
  state.counters["target_examined"] = stats.targetExamined.load() / queries;
  state.counters["budget"] = stats.initialBudget.load() / queries;
  state.counters["selectivity"] = (double)selected / data.size;
  state.counters["nBase"] = (double)data.size;
  state.counters["queries"] = queries;
  state.counters["dims"] = data.dims;
}

void BM_VectorAnnDataset(benchmark::State& state) {
  ForceIvfGuard forceIvf;
  const VectorDataset* ds = VectorDataset::get();
  if (skipBenchIfDataMissing(state, ds != nullptr, "LUXIR_VECTOR_DATASET")) return;
  int32_t codec = (int32_t)state.range(0);
  int32_t nprobe = (int32_t)state.range(1);
  int32_t cand = (int32_t)state.range(2);
  constexpr int32_t k = 10;

  CollectionHelper helper("main");
  static int32_t builtCodec = -1;
  static double buildSecs = 0;
  static double overlayBytes = 0;
  static int32_t overlaySegments = 0;
  static std::vector<std::vector<std::string>> truth;
  if (builtCodec != codec) {
    helper.clear();
    installVectorBenchSchema(helper.collection(), ds->dims, ds->metric, codec);
    int64_t perSeg = (ds->nBase + ds->segments - 1) / ds->segments;
    std::vector<Doc> docs;
    for (int64_t i = 0; i < ds->nBase; i++) {
      docs.push_back(flatdoc("id", "d" + std::to_string(i), "bench_v", ds->row(i)));
      if ((int64_t)docs.size() == 512 || i + 1 == ds->nBase) {
        helper.indexAll(docs);
        docs.clear();
      }
      if ((i + 1) % perSeg == 0 || i + 1 == ds->nBase) helper.commit();
    }
    // Production build policy and schema settings; only the aux commit is timed.
    auto start = std::chrono::steady_clock::now();
    helper.commit({"vec.bench_v"});
    buildSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    overlayBytes = 0;
    overlaySegments = 0;
    for (const auto& seg : helper.getIndexWriter()->stats(true).segmentStats) {
      for (const auto& overlay : seg.overlays) overlayBytes += (double)overlay.bytes;
      overlaySegments += seg.overlays.empty() ? 0 : 1;
    }
    builtCodec = codec;
    if (truth.empty()) {
      for (int32_t q = 0; q < ds->nQueries; q++) {
        auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v", ds->query(q), k,
                                    /*nprobe=*/0, /*refineCandidates=*/0, /*exact=*/true);
        req->execute(false);
        std::vector<std::string> ids;
        std::string error;
        bool ok = responseIds(*req, ids, error);
        req->done();
        if (!ok) {
          state.SkipWithError(error.c_str());
          return;
        }
        truth.push_back(std::move(ids));
      }
    }
  }

  std::vector<std::vector<float>> queries;
  for (int32_t q = 0; q < ds->nQueries; q++) queries.push_back(ds->query(q));
  double recallSum = 0;
  for (int32_t q = 0; q < ds->nQueries; q++) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v", queries[(size_t)q], k,
                                nprobe, cand);
    req->execute(false);
    std::vector<std::string> ids;
    std::string error;
    bool ok = responseIds(*req, ids, error);
    req->done();
    if (!ok) {
      state.SkipWithError(error.c_str());
      return;
    }
    int32_t hits = 0;
    for (const auto& id : ids) hits += std::ranges::count(truth[(size_t)q], id) > 0 ? 1 : 0;
    recallSum += (double)hits / (double)truth[(size_t)q].size();
  }

  int32_t queryOrd = 0;
  for (auto _ : state) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v", queries[(size_t)queryOrd],
                                k, nprobe, cand);
    req->execute(false);
    std::vector<std::string> ids;
    std::string error;
    bool ok = responseIds(*req, ids, error);
    req->done();
    if (!ok) {
      state.SkipWithError(error.c_str());
      break;
    }
    queryOrd = (queryOrd + 1) % ds->nQueries;
    benchmark::DoNotOptimize(ids);
  }

  state.SetLabel(benchCodecName(codec));
  state.counters["recall"] = recallSum / (double)ds->nQueries;
  state.counters["buildSecs"] = buildSecs;
  state.counters["bytesPerVec"] = overlayBytes / (double)ds->nBase;
  state.counters["overlaySegs"] = (double)overlaySegments;
  state.counters["nBase"] = (double)ds->nBase;
  state.counters["dims"] = (double)ds->dims;
  state.counters["nprobe"] = (double)nprobe;
  state.counters["cand"] = (double)cand;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

}  // namespace

// Real-corpus codec comparison (see BM_VectorAnnDataset); needs
// LUXIR_VECTOR_DATASET, so it is a measurement-only benchmark.
static void datasetSweep(benchmark::Benchmark* b, int64_t codec) {
  for (int64_t nprobe : {0, 32, 64, 128}) {
    for (int64_t cand : {0, 200, 500}) b->Args({codec, nprobe, cand});
  }
}
TUNING_BENCHMARK(BM_VectorAnnDataset)->ArgNames({"codec", "nprobe", "cand"})
    ->Apply([](benchmark::Benchmark* b) {
      datasetSweep(b, RABITQ1);
      datasetSweep(b, RABITQ2);
      datasetSweep(b, RABITQ4);
      datasetSweep(b, PQ);
      datasetSweep(b, PQ_D8);
    })->UseRealTime()->MinTime(2.0);

// Plain BENCHMARK: manual time is incompatible with the UseRealTime() that
// LUXIR_BENCHMARK appends.
BENCHMARK(BM_VectorIvfBuild)->ArgNames({"codec", "nDocs", "dims", "segments"})
    ->Args({RABITQ1, 50'000, 64, 4})->Args({PQ, 50'000, 64, 4})->Args({RABITQ4, 50'000, 64, 4})
    // Both training and add exceed the production 16 MiB transform chunk.
    ->Args({RABITQ1, 50'000, 128, 1})->Args({PQ, 50'000, 128, 1})
    ->UseManualTime()->Iterations(3);
// Production-shaped corpora for build-parallelism measurements: at 768 dims
// L2 PQ takes M=32 (24-wide sub-vectors), the BLAS encoding path, and every
// GEMM step is far above FAISS's BLAS threshold.  Compare against
// LUXIR_BENCH_IVF_MIN_ROWS=1000000000 (serial GEMMs on the calling thread).
TUNING_BENCHMARK(BM_VectorIvfBuild)->ArgNames({"codec", "nDocs", "dims", "segments"})
    ->Args({PQ, 300'000, 768, 1})->Args({RABITQ1, 300'000, 768, 1})
    ->UseManualTime()->Iterations(2);
BENCHMARK(BM_VectorIvfIncrementalBuild)->ArgName("aboveThresholdFlush")
    ->Arg(0)->Arg(1)->UseManualTime()->Iterations(3);

LUXIR_BENCHMARK(BM_VectorKnnFiltered)->ArgNames({"codec", "permyriad", "correlation", "alpha100", "mode"})
    ->Apply([](benchmark::Benchmark* b) {
      for (int codec : {PQ, RABITQ2, RABITQ1}) for (int selectivity : {100, 1000})
        for (int correlation : {0, 1, 2, 3}) b->Args({codec, selectivity, correlation, 50, 0});
    });

TUNING_BENCHMARK(BM_VectorKnnFiltered)->UseRealTime()->ArgNames({"codec", "permyriad", "correlation", "alpha100", "mode"})
    ->Apply([](benchmark::Benchmark* b) {
      for (int codec : {PQ, RABITQ2, RABITQ1})
        for (int selectivity : {1, 3, 10, 30, 100, 300, 1000, 3000})
          for (int correlation : {0, 1, 2, 3}) {
            for (int alpha : {0, 25, 50, 100}) b->Args({codec, selectivity, correlation, alpha, 1});
            b->Args({codec, selectivity, correlation, 50, 0});
            b->Args({codec, selectivity, correlation, 50, 2});
          }
    });

LUXIR_BENCHMARK(BM_VectorKnnFlatDomain)->ArgNames({"shape", "basis_points", "arm"})
    ->Apply([](benchmark::Benchmark* b) {
      for (int shape : {0, 1, 2}) for (int density : {1, 10, 100, 1000, 5000, 10000})
        for (int arm : {0, 1}) b->Args({shape, density, arm});
    });

LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, single_column, false, false)->Arg(50'000);
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, single_ivf, false, true)->Arg(50'000);
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, multi_column, true, false)->Arg(50'000);
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, multi_ivf, true, true)->Arg(50'000);
// Deepen-latency point: the skew corpus (see makeVectorDoc) makes round-1
// collapse underfill the doc target, forcing depth rounds (folds counter
// == 1).  IVF-only: the flat column engine doc-collapses during its scan
// and so never deepens on multiplicity.
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, multi_ivf_skew, true, true, true)->Arg(50'000);

// Two sweeps over (nprobe, refine); 0 = production default for either knob.
// nprobe sweep at default refine: breadth axis, up to exhaustive-over-lists.
// refine sweep at default nprobe: candidate-selection axis (the 2026-06
// IVF+PQ measurements showed the recall ceiling is refine-bound, not
// probe-bound).  The (exhaustive, 160) point checks how close the ceiling
// gets to 1.0 when both knobs are generous.  Registered per codec, grouped so
// consecutive instances reuse the built index.
static void recallSweep(benchmark::Benchmark* b, int64_t codec) {
  b->Args({0, 0, 10, codec})->Args({1, 0, 10, codec})->Args({4, 0, 10, codec})
    ->Args({16, 0, 10, codec})->Args({64, 0, 10, codec})->Args({1 << 20, 0, 10, codec})
    ->Args({0, 10, 10, codec})->Args({0, 80, 10, codec})->Args({0, 100, 10, codec})
    ->Args({0, 120, 10, codec})->Args({0, 140, 10, codec})->Args({0, 160, 10, codec})
    ->Args({1 << 20, 160, 10, codec})
    // k sweep at default nprobe/refine: checks the affine sizing across k.
    // Note recall granularity is 1/(16*k): coarse at k=1.
    ->Args({0, 0, 1, codec})->Args({0, 0, 100, codec})->Args({0, 0, 1000, codec});
}
LUXIR_BENCHMARK(BM_VectorKnnRecall)->Apply(forceIvfBenchmark)->ArgNames({"nprobe", "cand", "k", "codec"})
    ->Apply([](benchmark::Benchmark* b) {
      recallSweep(b, RABITQ1);
      recallSweep(b, PQ);
    });

// Parallel-execution latency points: default knobs, a wide-breadth probe
// (more selected lists = more scan tasks), and a deep-k pool (bigger
// rescore buckets).
LUXIR_BENCHMARK(BM_VectorKnnRecallParallel)->Apply(forceIvfBenchmark)->ArgNames({"nprobe", "cand", "k", "codec"})
    ->Args({0, 0, 10, RABITQ1})->Args({16, 0, 10, RABITQ1})->Args({1 << 20, 0, 10, RABITQ1})
    ->Args({0, 0, 1000, RABITQ1});

LUXIR_BENCHMARK(BM_VectorKnnRecallThreads)->Apply(forceIvfBenchmark)->ArgNames({"nprobe", "cand", "k", "codec"})
    ->Args({0, 0, 10, RABITQ1})->ThreadRange(1, 32);
LUXIR_BENCHMARK(BM_VectorKnnRecallThreadsParallel)->Apply(forceIvfBenchmark)->ArgNames({"nprobe", "cand", "k", "codec"})
    ->Args({0, 0, 10, RABITQ1})->ThreadRange(1, 32);

// Matched query-path cells: same clustered corpus, explicit probe and pool
// budgets. Exact requests use the column while sharing the PQ-built fixture.
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnnRecallBody, pq_serial, false)->Apply(forceIvfBenchmark)
    ->ArgNames({"nprobe", "cand", "k", "codec"})->Args({4, 160, 10, PQ});
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnnRecallBody, pq_parallel, true)->Apply(forceIvfBenchmark)
    ->ArgNames({"nprobe", "cand", "k", "codec"})->Args({4, 160, 10, PQ});
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnnRecallBody, flat_serial, false, true)->Apply(forceIvfBenchmark)
    ->ArgNames({"nprobe", "cand", "k", "codec"})->Args({4, 160, 10, PQ});
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnnRecallBody, flat_parallel, true, true)->Apply(forceIvfBenchmark)
    ->ArgNames({"nprobe", "cand", "k", "codec"})->Args({4, 160, 10, PQ});
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnnRecallBody, rabitq_groups, false, false, true)->Apply(forceIvfBenchmark)
    ->ArgNames({"nprobe", "cand", "k", "codec"})->Args({4, 160, 10, RABITQ1});
