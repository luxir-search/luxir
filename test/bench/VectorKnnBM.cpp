// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
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

using namespace luxir;
using namespace luxir::test;

namespace api = luxir::api;

namespace {

void installVectorBenchSchema(Collection& col, int32_t dims,
                              api::VectorMetric metric = api::VectorMetric::IP) {
  SchemaBuilder b;

  auto& single = b.templ("_v");
  single.type = api::FieldDef_::FieldClass::VECTOR;
  single.column = true;
  single.dims = dims;
  single.metric = metric;

  auto& multi = b.templ("_vs");
  multi.type = api::FieldDef_::FieldClass::VECTOR;
  multi.column = true;
  multi.multi = true;
  multi.dims = dims;
  multi.metric = metric;

  b.set(col);
}

float nextFloat(SplitMix64& rng) {
  return ((float)((int32_t)rng.rint((int64_t)2001) - 1000)) / 1000.0f;
}

struct IvfPqBenchGuard {
  bool savedIvfPq;
  int32_t savedNList;
  int32_t savedM;
  int32_t savedBits;
  int32_t savedNProbe;
  int64_t savedMinTraining;
  int64_t savedBuildThreshold;

  IvfPqBenchGuard()
    : savedIvfPq(VectorIndexBuilder::buildFaissIvfPqAuxIndexes),
      savedNList(VectorIndexBuilder::ivfPqNList),
      savedM(VectorIndexBuilder::ivfPqM),
      savedBits(VectorIndexBuilder::ivfPqBits),
      savedNProbe(VectorIndexBuilder::ivfPqDefaultNProbe),
      savedMinTraining(VectorIndexBuilder::ivfPqMinTrainingVectors),
      savedBuildThreshold(VectorIndexBuilder::ivfPqBuildThresholdScanCost) {
    VectorIndexBuilder::buildFaissIvfPqAuxIndexes = true;
    VectorIndexBuilder::ivfPqBuildThresholdScanCost = 0;
    if (luxir::unit_tests) {
      // Tiny corpus in unit-test mode: shrink so IVF+PQ actually trains and
      // the ANN path gets coverage. Real runs keep production IVF+PQ params
      // but force eligibility so the benchmark measures the aux path.
      VectorIndexBuilder::ivfPqNList = 4;
      VectorIndexBuilder::ivfPqM = 2;
      VectorIndexBuilder::ivfPqBits = 1;
      VectorIndexBuilder::ivfPqMinTrainingVectors = 16;
    }
  }

  ~IvfPqBenchGuard() {
    VectorIndexBuilder::buildFaissIvfPqAuxIndexes = savedIvfPq;
    VectorIndexBuilder::ivfPqNList = savedNList;
    VectorIndexBuilder::ivfPqM = savedM;
    VectorIndexBuilder::ivfPqBits = savedBits;
    VectorIndexBuilder::ivfPqDefaultNProbe = savedNProbe;
    VectorIndexBuilder::ivfPqMinTrainingVectors = savedMinTraining;
    VectorIndexBuilder::ivfPqBuildThresholdScanCost = savedBuildThreshold;
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
    IvfPqBenchGuard guard;
    helper.commit({std::string("vec.")
                   + (valuesPerDoc == 1 && !skew ? "bench_v" : "bench_vs")});
  }
}

LocalReq* makeKnnBenchReq(SearchEngine& engine, std::string_view field,
                          const std::vector<float>& queryVec, int32_t k,
                          int32_t nprobe = 0, int32_t refineCandidates = 0,
                          bool exact = false) {
  auto* req = LocalReq::create(engine);
  req->collection("main").requestId("vector-bench");

  auto& cur = req->topDocs();
  cur.limit(k).getNumber().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, queryVec, k, nprobe, exact, refineCandidates);

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

// ---------------------------------------------------------------------------
// IVF+PQ recall benchmark.
//
// Measures recall@k of the production IVF+PQ defaults against ground truth
// from the exact contract path (KnnQuery.exact), through the same request
// pipeline - same filters, collapse, and score scale, so the only variable is
// the index.  The nprobe arg sweeps breadth (0 = the index build default,
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

std::vector<int32_t> clusteredDocsPerSeg(int64_t nDocs) {
  int64_t nSegs = luxir::unit_tests ? 2 : 4;
  std::vector<int32_t> docsPerSeg((size_t)nSegs);
  for (int64_t seg = 0; seg < nSegs; seg++) {
    docsPerSeg[(size_t)seg] = (int32_t)(nDocs / nSegs + (seg < nDocs % nSegs ? 1 : 0));
  }
  return docsPerSeg;
}

// Reuse check (same pattern as QueryBM / FacetBM): segment shape must match,
// but shape alone is ambiguous here - BM_VectorKnn builds same-sized indexes
// with uniform IP data - so also verify the aux is our clustered IVF+PQ / L2
// build via its metadata.
bool clusteredIndexReusable(CollectionHelper& helper, std::span<const int32_t> docsPerSeg) {
  if (!helper.indexMatchesShape(docsPerSeg)) return false;
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  for (const auto& seg : reader->segments()) {
    auto aux = seg.getAuxReader("vec.bench_v");
    if (!aux) return false;
    auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
    if (vaux == nullptr ||
        vaux->getEngine() != VectorAuxMeta::ENGINE_IVFPQ ||
        vaux->getMetric() != (int32_t)api::VectorMetric::L2) {
      return false;
    }
  }
  return true;
}

void buildClusteredVectorIndex(CollectionHelper& helper, int32_t dims,
                               int32_t nClusters, std::span<const int32_t> docsPerSeg) {
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

void BM_VectorKnnRecallBody(benchmark::State& state, bool parallelExec) {
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
    reuseIndex = clusteredIndexReusable(helper, docsPerSeg);
    if (!reuseIndex) {
      IvfPqBenchGuard guard;
      buildClusteredVectorIndex(helper, dims, nClusters, docsPerSeg);
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
  double recallSum = 0;
  for (int32_t q = 0; q < nQueries; q++) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v",
                                queries[(size_t)q], k, nprobe, cand);
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

  // Timed loop measures latency only, cycling queries for realistic variety.
  // parallelExec runs each request on the TBB task group (intra-query scan /
  // rescore tasks); results are identical either way, only latency moves.
  int32_t queryOrd = 0;
  for (auto _ : state) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v",
                                queries[(size_t)queryOrd], k, nprobe, cand);
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
  state.counters["reused"] = Counter(reuseIndex ? 1.0 : 0.0, Counter::kAvgThreads);
  state.counters["k"] = Counter((double)k, Counter::kAvgThreads);
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

// Build-phase cost of the IVF+PQ aux index.  Manual timing covers ONLY the
// aux commit (train + encode + serialize) - the marginal cost of the ANN
// index over plain column storage.  Doc indexing wall time is reported as a
// counter for context.  Fixed iteration count: each iteration is a full
// rebuild, so let it be expensive but bounded.
void BM_VectorIvfPqBuild(benchmark::State& state) {
  int64_t nDocs = luxir::unit_tests
      ? LuxirTest::scaleTestWork(240)
      : 50'000;
  int32_t dims = luxir::unit_tests ? 16 : 64;
  int32_t nClusters = luxir::unit_tests ? 8 : 100;

  IvfPqBenchGuard guard;
  CollectionHelper helper("main");
  std::vector<int32_t> docsPerSeg = clusteredDocsPerSeg(nDocs);

  double indexDocsSecs = 0;
  for (auto _ : state) {
    // Untimed (for the manual clock): clear, schema, index docs, segment
    // commits.  buildClusteredVectorIndex's final aux commit is what we
    // want isolated, so inline its steps here.
    auto wallStart = std::chrono::steady_clock::now();
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
    indexDocsSecs += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wallStart).count();

    // Timed: the aux build commit only.
    BenchTimer timer(state);
    helper.commit({"vec.bench_v"});
  }

  state.counters["nDocs"] = (double)nDocs;
  state.counters["dims"] = (double)dims;
  state.counters["docIndexSecs"] =
      state.iterations() > 0 ? indexDocsSecs / (double)state.iterations() : 0.0;
  state.counters["vecPerSec"] = benchmark::Counter(
      (double)nDocs * (double)state.iterations(), benchmark::Counter::kIsRate);
}

void BM_VectorIvfPqIncrementalBuild(benchmark::State& state) {
  bool aboveThresholdFlush = state.range(0) != 0;
  int32_t dims = luxir::unit_tests ? 16 : 64;
  int32_t nClusters = luxir::unit_tests ? 8 : 100;
  int64_t flushDocs = aboveThresholdFlush
    ? (luxir::unit_tests ? 120 : 25'000)
    : (luxir::unit_tests ? 4 : 128);
  int64_t threshold = luxir::unit_tests
    ? (int64_t)dims * 64
    : 1'000'000;

  IvfPqBenchGuard guard;
  VectorIndexBuilder::ivfPqBuildThresholdScanCost = threshold;
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

    VectorIndexBuilder::ivfPqBuildCountForTests.store(0, std::memory_order_relaxed);
    VectorIndexBuilder::ivfPqMergeBuildCountForTests.store(0, std::memory_order_relaxed);
    BenchTimer timer(state);
    helper.commit({"vec.bench_v"});
    annBuilds += VectorIndexBuilder::ivfPqBuildCountForTests.load(std::memory_order_relaxed);
  }

  state.counters["flushDocs"] = (double)flushDocs;
  state.counters["aboveThreshold"] = aboveThresholdFlush ? 1.0 : 0.0;
  state.counters["annBuilds"] =
      state.iterations() > 0 ? (double)annBuilds / (double)state.iterations() : 0.0;
  state.counters["setupSecs"] =
      state.iterations() > 0 ? setupSecs / (double)state.iterations() : 0.0;
}

}  // namespace

// Plain BENCHMARK: manual time is incompatible with the UseRealTime() that
// LUXIR_BENCHMARK appends.
BENCHMARK(BM_VectorIvfPqBuild)->UseManualTime()->Iterations(3);
BENCHMARK(BM_VectorIvfPqIncrementalBuild)->ArgName("aboveThresholdFlush")
    ->Arg(0)->Arg(1)->UseManualTime()->Iterations(3);

LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, single_column, false, false)->Arg(50'000);
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, single_ivfpq, false, true)->Arg(50'000);
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, multi_column, true, false)->Arg(50'000);
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, multi_ivfpq, true, true)->Arg(50'000);
// Deepen-latency point: the skew corpus (see makeVectorDoc) makes round-1
// collapse underfill the doc target, forcing depth rounds (folds counter
// == 1).  IVF-only: the flat column engine doc-collapses during its scan
// and so never deepens on multiplicity.
LUXIR_BENCHMARK_CAPTURE(BM_VectorKnn, multi_ivfpq_skew, true, true, true)->Arg(50'000);

// Two sweeps over (nprobe, refine); 0 = production default for either knob.
// nprobe sweep at default refine: breadth axis, up to exhaustive-over-lists.
// refine sweep at default nprobe: PQ candidate-selection axis (the 2026-06
// measurements showed the recall ceiling is refine-bound, not probe-bound).
// The (exhaustive, 16) point checks how close the ceiling gets to 1.0 when
// both knobs are generous.
LUXIR_BENCHMARK(BM_VectorKnnRecall)->ArgNames({"nprobe", "cand", "k"})
    ->Args({0, 0, 10})->Args({1, 0, 10})->Args({4, 0, 10})->Args({16, 0, 10})
    ->Args({64, 0, 10})->Args({1 << 20, 0, 10})
    ->Args({0, 10, 10})->Args({0, 80, 10})->Args({0, 100, 10})->Args({0, 120, 10})
    ->Args({0, 140, 10})->Args({0, 160, 10})
    ->Args({1 << 20, 160, 10})
    // k sweep at default nprobe/refine: checks the affine sizing across k.
    // Note recall granularity is 1/(16*k): coarse at k=1.
    ->Args({0, 0, 1})->Args({0, 0, 100})->Args({0, 0, 1000});

// Parallel-execution latency points: default knobs, a wide-breadth probe
// (more selected lists = more scan tasks), and a deep-k pool (bigger
// rescore buckets).
LUXIR_BENCHMARK(BM_VectorKnnRecallParallel)->ArgNames({"nprobe", "cand", "k"})
    ->Args({0, 0, 10})->Args({16, 0, 10})->Args({1 << 20, 0, 10})
    ->Args({0, 0, 1000});

LUXIR_BENCHMARK(BM_VectorKnnRecallThreads)->ArgNames({"nprobe", "cand", "k"})
    ->Args({0, 0, 10})->ThreadRange(1, 32);
LUXIR_BENCHMARK(BM_VectorKnnRecallThreadsParallel)->ArgNames({"nprobe", "cand", "k"})
    ->Args({0, 0, 10})->ThreadRange(1, 32);
