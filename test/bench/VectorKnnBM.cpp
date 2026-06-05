#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bench/solux_bench.h"
#include "protos/solux_types.pb.h"
#include "solux/index/VectorIndexBuilder.h"
#include "solux/schema/Schema.h"
#include "solux/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

namespace {

struct FaissFlatBuildGuard {
  bool saved;

  explicit FaissFlatBuildGuard(bool enabled) : saved(VectorIndexBuilder::buildFaissFlatAuxIndexes) {
    VectorIndexBuilder::buildFaissFlatAuxIndexes = enabled;
  }

  ~FaissFlatBuildGuard() {
    VectorIndexBuilder::buildFaissFlatAuxIndexes = saved;
  }
};

void installVectorBenchSchema(Collection& col, int32_t dims) {
  proto::SchemaDef def;

  auto* single = def.add_fields();
  single->set_name("_v");
  single->set_field_class(proto::FieldDef::VECTOR);
  single->set_abstract(true);
  single->set_column_stored(true);
  single->mutable_vector()->set_dims(dims);
  single->mutable_vector()->set_metric(proto::VectorParams::IP);

  auto* multi = def.add_fields();
  multi->set_name("_vs");
  multi->set_field_class(proto::FieldDef::VECTOR);
  multi->set_abstract(true);
  multi->set_column_stored(true);
  multi->set_multi_valued(true);
  multi->mutable_vector()->set_dims(dims);
  multi->mutable_vector()->set_metric(proto::VectorParams::IP);

  auto base = Schema::createDefaultSchema();
  col.setSchema(Schema::fromProto(def, base.get()));
}

float nextFloat(SplitMix64& rng) {
  return ((float)((int32_t)rng.rint((int64_t)2001) - 1000)) / 1000.0f;
}

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
  FaissFlatBuildGuard guard(buildFaissAux);
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
    helper.commit({std::string("vec.") + (valuesPerDoc == 1 ? "bench_v" : "bench_vs")});
  }
}

LocalReq* makeKnnBenchReq(SearchEngine& engine, std::string_view field,
                          const std::vector<float>& queryVec, int32_t k) {
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

}  // namespace

SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, single_column, false, false)->Arg(50'000);
SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, single_faiss_flat, false, true)->Arg(50'000);
SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, multi_column, true, false)->Arg(50'000);
SOLUX_BENCHMARK_CAPTURE(BM_VectorKnn, multi_faiss_flat, true, true)->Arg(50'000);
