#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "bench/solux_bench.h"
#include "protos/solux_types.pb.h"
#include "solux/index/VectorIndexBuilder.h"
#include "solux/reader/VectorAuxReader.h"
#include "solux/schema/Schema.h"
#include "solux/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

namespace {

void installVectorBenchSchema(Collection& col, int32_t dims,
                              proto::VectorParams::Metric metric = proto::VectorParams::IP) {
  proto::SchemaDef def;

  auto* single = def.add_fields();
  single->set_name("_v");
  single->set_field_class(proto::FieldDef::VECTOR);
  single->set_abstract(true);
  single->set_column_stored(true);
  single->mutable_vector()->set_dims(dims);
  single->mutable_vector()->set_metric(metric);

  auto* multi = def.add_fields();
  multi->set_name("_vs");
  multi->set_field_class(proto::FieldDef::VECTOR);
  multi->set_abstract(true);
  multi->set_column_stored(true);
  multi->set_multi_valued(true);
  multi->mutable_vector()->set_dims(dims);
  multi->mutable_vector()->set_metric(metric);

  auto base = Schema::createDefaultSchema();
  col.setSchema(Schema::fromProto(def, base.get()));
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
    if (solux::unit_tests) {
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

Doc makeVectorDoc(int64_t docId, int32_t dims, int32_t valuesPerDoc) {
  std::string id = "d" + std::to_string(docId);
  if (valuesPerDoc == 1) {
    return flatdoc("id", id, "bench_v", makeVector(docId, dims, 0));
  }

  std::vector<std::vector<float>> values;
  values.reserve((size_t)valuesPerDoc);
  for (int32_t i = 0; i < valuesPerDoc; i++) {
    values.push_back(makeVector(docId, dims, i));
  }
  return flatdoc("id", id, "bench_vs", values);
}

void indexVectorBatch(CollectionHelper& helper, int64_t startDoc, int64_t count,
                      int32_t dims, int32_t valuesPerDoc) {
  constexpr int64_t batchSize = 256;
  std::vector<Doc> docs;
  docs.reserve((size_t)batchSize);

  for (int64_t i = 0; i < count; i++) {
    docs.push_back(makeVectorDoc(startDoc + i, dims, valuesPerDoc));
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
                           int32_t valuesPerDoc, bool buildFaissAux) {
  helper.clear();
  installVectorBenchSchema(helper.collection(), dims);

  int64_t nSegs = solux::unit_tests ? 2 : 4;
  int64_t baseSegDocs = nDocs / nSegs;
  int64_t remainder = nDocs % nSegs;
  int64_t docId = 0;
  for (int64_t seg = 0; seg < nSegs; seg++) {
    int64_t segDocs = baseSegDocs + (seg < remainder ? 1 : 0);
    indexVectorBatch(helper, docId, segDocs, dims, valuesPerDoc);
    helper.commit();
    docId += segDocs;
  }

  if (buildFaissAux) {
    IvfPqBenchGuard guard;
    helper.commit({std::string("vec.") + (valuesPerDoc == 1 ? "bench_v" : "bench_vs")});
  }
}

LocalReq* makeKnnBenchReq(SearchEngine& engine, std::string_view field,
                          const std::vector<float>& queryVec, int32_t k,
                          int32_t nprobe = 0, int32_t refineCandidates = 0,
                          bool exact = false) {
  auto* req = LocalReq::create(engine);
  req->proto.mutable_collection()->add_name("main");
  req->proto.set_request_id("vector-bench");

  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_limit(k);
  topDocs.set_get_number(true);
  topDocs.mutable_fields()->Add("id");

  auto& knn = *topDocs.mutable_query()->mutable_knn();
  knn.set_field(field);
  knn.set_k(k);
  if (nprobe > 0) knn.set_nprobe(nprobe);
  if (refineCandidates > 0) knn.set_refine_candidates(refineCandidates);
  if (exact) knn.set_exact(true);
  auto& f32 = *knn.mutable_query()->mutable_f32();
  for (float v : queryVec) {
    f32.add_v(v);
  }

  return req;
}

bool fingerprint(LocalReq& req, uint64_t& fp, std::string& error) {
  if (req.responses.empty()) {
    error = "missing search response";
    return false;
  }
  const auto& response = req.responses[0]->proto;
  if (!response.error().empty()) {
    error = response.error();
    return false;
  }
  const auto& docs = response.ops().at("q").docs();
  fp = (uint64_t)docs.matches();
  const auto& ids = docs.columns().at("id").col_s();
  for (const auto& id : ids.v()) {
    fp = fp * 131 + (uint32_t)java_string_hashcode(id);
  }
  return true;
}

void BM_VectorKnn(benchmark::State& state, bool multiValued, bool faissAux) {
  int64_t nDocs = solux::unit_tests ? 240 : state.range(0);
  int32_t dims = solux::unit_tests ? 16 : 64;
  int32_t valuesPerDoc = multiValued ? 4 : 1;
  int32_t k = 10;

  CollectionHelper helper("main");
  buildVectorBenchIndex(helper, nDocs, dims, valuesPerDoc, faissAux);
  std::vector<float> query = makeQueryVector(dims);
  std::string field = multiValued ? "bench_vs" : "bench_v";

  uint64_t expectedFp = 0;
  bool haveExpectedFp = false;
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
  int64_t nSegs = solux::unit_tests ? 2 : 4;
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
  auto reader = helper.getIndexWriter()->getIndexReader();
  for (const auto& seg : reader->segments()) {
    auto aux = seg.getAuxReader("vec.bench_v");
    if (!aux) return false;
    auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
    if (vaux == nullptr ||
        vaux->getEngine() != VectorAuxMeta::ENGINE_IVFPQ ||
        vaux->getMetric() != (int32_t)proto::VectorParams::L2) {
      return false;
    }
  }
  return true;
}

void buildClusteredVectorIndex(CollectionHelper& helper, int32_t dims,
                               int32_t nClusters, std::span<const int32_t> docsPerSeg) {
  helper.clear();
  installVectorBenchSchema(helper.collection(), dims, proto::VectorParams::L2);

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
  if (!response.error().empty()) {
    error = response.error();
    return false;
  }
  auto opIt = response.ops().find("q");
  if (opIt == response.ops().end()) {
    error = "missing q response op";
    return false;
  }
  const auto& docs = opIt->second.docs();
  auto idIt = docs.columns().find("id");
  if (idIt == docs.columns().end()) {
    error = "missing id column";
    return false;
  }
  const auto& ids = idIt->second.col_s();
  out.assign(ids.v().begin(), ids.v().end());
  return true;
}

void BM_VectorKnnRecall(benchmark::State& state) {
  int64_t nDocs = solux::unit_tests ? 240 : 50'000;
  int32_t dims = solux::unit_tests ? 16 : 64;
  int32_t nClusters = solux::unit_tests ? 8 : 100;
  int32_t nQueries = 16;
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
  int32_t queryOrd = 0;
  for (auto _ : state) {
    auto* req = makeKnnBenchReq(helper.getSearchEngine(), "bench_v",
                                queries[(size_t)queryOrd], k, nprobe, cand);
    req->execute(false);
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

// Throughput wrapper: same benchmark at the production defaults, run on
// 1..32 concurrent closed-loop threads.  rate = aggregate QPS.  Registered
// as a separate function so the thread sweep does not multiply the full
// (nprobe, refine, k) grid.
void BM_VectorKnnRecallThreads(benchmark::State& state) {
  BM_VectorKnnRecall(state);
}

// Build-phase cost of the IVF+PQ aux index.  Manual timing covers ONLY the
// aux commit (train + encode + serialize) - the marginal cost of the ANN
// index over plain column storage.  Doc indexing wall time is reported as a
// counter for context.  Fixed iteration count: each iteration is a full
// rebuild, so let it be expensive but bounded.
void BM_VectorIvfPqBuild(benchmark::State& state) {
  int64_t nDocs = solux::unit_tests ? 240 : 50'000;
  int32_t dims = solux::unit_tests ? 16 : 64;
  int32_t nClusters = solux::unit_tests ? 8 : 100;

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
    installVectorBenchSchema(helper.collection(), dims, proto::VectorParams::L2);
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
  int32_t dims = solux::unit_tests ? 16 : 64;
  int32_t nClusters = solux::unit_tests ? 8 : 100;
  int64_t flushDocs = aboveThresholdFlush
    ? (solux::unit_tests ? 120 : 25'000)
    : (solux::unit_tests ? 4 : 128);
  int64_t threshold = solux::unit_tests
    ? (int64_t)dims * 64
    : 1'000'000;

  IvfPqBenchGuard guard;
  VectorIndexBuilder::ivfPqBuildThresholdScanCost = threshold;
  CollectionHelper helper("main");
  std::vector<int32_t> docsPerSeg = solux::unit_tests
    ? std::vector<int32_t>{120, 120}
    : std::vector<int32_t>{25'000, 25'000};

  double setupSecs = 0;
  int64_t annBuilds = 0;
  for (auto _ : state) {
    auto setupStart = std::chrono::steady_clock::now();
    helper.clear();
    installVectorBenchSchema(helper.collection(), dims, proto::VectorParams::L2);
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

    VectorIndexBuilder::ivfPqBuildCountForTests = 0;
    BenchTimer timer(state);
    helper.commit({"vec.bench_v"});
    annBuilds += VectorIndexBuilder::ivfPqBuildCountForTests;
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
// SOLUX_BENCHMARK appends.
BENCHMARK(BM_VectorIvfPqBuild)->UseManualTime()->Iterations(3);
BENCHMARK(BM_VectorIvfPqIncrementalBuild)->ArgName("aboveThresholdFlush")
    ->Arg(0)->Arg(1)->UseManualTime()->Iterations(3);

SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, single_column, false, false)->Arg(50'000);
SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, single_ivfpq, false, true)->Arg(50'000);
SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, multi_column, true, false)->Arg(50'000);
SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, multi_ivfpq, true, true)->Arg(50'000);

// Two sweeps over (nprobe, refine); 0 = production default for either knob.
// nprobe sweep at default refine: breadth axis, up to exhaustive-over-lists.
// refine sweep at default nprobe: PQ candidate-selection axis (the 2026-06
// measurements showed the recall ceiling is refine-bound, not probe-bound).
// The (exhaustive, 16) point checks how close the ceiling gets to 1.0 when
// both knobs are generous.
SOLUX_BENCHMARK(BM_VectorKnnRecall)->ArgNames({"nprobe", "cand", "k"})
    ->Args({0, 0, 10})->Args({1, 0, 10})->Args({4, 0, 10})->Args({16, 0, 10})
    ->Args({64, 0, 10})->Args({1 << 20, 0, 10})
    ->Args({0, 10, 10})->Args({0, 80, 10})->Args({0, 100, 10})->Args({0, 120, 10})
    ->Args({0, 140, 10})->Args({0, 160, 10})
    ->Args({1 << 20, 160, 10})
    // k sweep at default nprobe/refine: checks the affine sizing across k.
    // Note recall granularity is 1/(16*k): coarse at k=1.
    ->Args({0, 0, 1})->Args({0, 0, 100})->Args({0, 0, 1000});

SOLUX_BENCHMARK(BM_VectorKnnRecallThreads)->ArgNames({"nprobe", "cand", "k"})
    ->Args({0, 0, 10})->ThreadRange(1, 32);
