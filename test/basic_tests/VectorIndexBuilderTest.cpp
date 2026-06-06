#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "solux/index/UpdateMessage.h"
#include "solux/index/VectorIndexBuilder.h"
#include "solux/reader/VectorAuxReader.h"
#include "solux/schema/Schema.h"
#include "solux/server/SoluxNode.h"
#include "solux/store/Directory.h"
#include "solux/reader/Postings.h"
#include "solux/util/Signal.h"
#include "protos/solux_types.pb.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/MetricType.h>
#include <faiss/index_io.h>
#include <faiss/impl/io.h>

#include <google/protobuf/arena.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <cstring>
#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

using namespace solux;
using namespace solux::test;

class VectorIndexBuilderTest : public SoluxTest {
protected:
  void SetUp() override {
    // Reset to default schema; tests selectively install metric-bearing schemas
    // via enableL2OnVecSuffix.  Persisted schemas from prior tests would otherwise
    // leak in.
    auto col = soluxNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }

  struct IvfPqGuard {
    bool savedIvfPq;
    int32_t savedNList;
    int32_t savedM;
    int32_t savedBits;
    int32_t savedNProbe;
    int64_t savedMinTraining;
    int64_t savedBuildThreshold;

    IvfPqGuard(int32_t nlist, int32_t m, int32_t bits,
               int32_t nprobe, int64_t minTraining)
      : savedIvfPq(VectorIndexBuilder::buildFaissIvfPqAuxIndexes),
        savedNList(VectorIndexBuilder::ivfPqNList),
        savedM(VectorIndexBuilder::ivfPqM),
        savedBits(VectorIndexBuilder::ivfPqBits),
        savedNProbe(VectorIndexBuilder::ivfPqDefaultNProbe),
        savedMinTraining(VectorIndexBuilder::ivfPqMinTrainingVectors),
        savedBuildThreshold(VectorIndexBuilder::ivfPqBuildThresholdScanCost) {
      VectorIndexBuilder::buildFaissIvfPqAuxIndexes = true;
      VectorIndexBuilder::ivfPqNList = nlist;
      VectorIndexBuilder::ivfPqM = m;
      VectorIndexBuilder::ivfPqBits = bits;
      VectorIndexBuilder::ivfPqDefaultNProbe = nprobe;
      VectorIndexBuilder::ivfPqMinTrainingVectors = minTraining;
      VectorIndexBuilder::ivfPqBuildThresholdScanCost = 0;
    }

    ~IvfPqGuard() {
      VectorIndexBuilder::buildFaissIvfPqAuxIndexes = savedIvfPq;
      VectorIndexBuilder::ivfPqNList = savedNList;
      VectorIndexBuilder::ivfPqM = savedM;
      VectorIndexBuilder::ivfPqBits = savedBits;
      VectorIndexBuilder::ivfPqDefaultNProbe = savedNProbe;
      VectorIndexBuilder::ivfPqMinTrainingVectors = savedMinTraining;
      VectorIndexBuilder::ivfPqBuildThresholdScanCost = savedBuildThreshold;
    }
  };
};

namespace {

// Reads s.olux and returns the parsed IndexInfo.  Caller owns the storage in `arena`.
proto::IndexInfo* readIndexInfo(Directory& dir, google::protobuf::Arena& arena) {
  auto file = dir.openFile(Postings::INDEX_INFO_FILE);
  EXPECT_NE(file, nullptr);
  auto* info = google::protobuf::Arena::Create<proto::IndexInfo>(&arena);
  auto is = file->getInputStream();
  google::protobuf::io::ArrayInputStream as(is.ptr(), (int)is.left());
  google::protobuf::io::CodedInputStream cs(&as);
  EXPECT_TRUE(info->ParseFromCodedStream(&cs));
  return info;
}

// Reads a faiss::IndexFlat from a directory file.
std::unique_ptr<faiss::Index> readFaissIndex(Directory& dir, std::string_view fname) {
  auto file = dir.openFile(fname);
  EXPECT_NE(file, nullptr);
  auto bytes = file->read();
  faiss::VectorIOReader r;
  r.data.assign((const uint8_t*)bytes.data(), (const uint8_t*)bytes.data() + bytes.size());
  return std::unique_ptr<faiss::Index>(faiss::read_index(&r));
}

VectorAuxMeta readVectorAuxMeta(const proto::AuxIndexInfo& aux) {
  return VectorAuxMeta::fromBytes(aux.opaque_meta(), aux.name());
}

std::vector<const proto::AuxIndexInfo*> vectorOverlays(const proto::IndexInfo* info) {
  std::vector<const proto::AuxIndexInfo*> out;
  for (const auto& seg : info->segments()) {
    for (const auto& overlay : seg.overlays()) {
      if (overlay.kind() == VectorIndexBuilder::KIND) out.push_back(&overlay);
    }
  }
  return out;
}

const proto::AuxIndexInfo& onlyVectorOverlay(const proto::IndexInfo* info) {
  auto overlays = vectorOverlays(info);
  EXPECT_EQ(overlays.size(), 1u);
  return *overlays[0];
}

// Install a schema where _v has metric=L2.
void enableL2OnVecSuffix(Collection& col) {
  proto::SchemaDef def;
  auto* f = def.add_fields();
  f->set_name("_v");
  f->set_field_class(proto::FieldDef::VECTOR);
  f->set_abstract(true);
  f->set_column_stored(true);
  f->mutable_vector()->set_metric(proto::VectorParams::L2);

  // fromProto with the existing schema as base preserves all built-in fields and
  // overrides _v with the metric-bearing definition.
  auto base = col.getSchema();
  col.setSchema(Schema::fromProto(def, base.get()));
}

std::vector<std::string> runKnnIds(SearchEngine& engine, std::string_view field,
                                  const std::vector<float>& queryVec, int32_t k,
                                  int32_t nprobe = 0, int32_t refineCandidates = 0,
                                  bool exact = false) {
  auto* req = LocalReq::create(engine);
  req->proto.mutable_collection()->add_name("main");
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_fields()->Add("id");
  auto& knn = *topDocs.mutable_query()->mutable_knn();
  knn.set_field(field);
  knn.set_k(k);
  if (nprobe > 0) knn.set_nprobe(nprobe);
  if (refineCandidates > 0) knn.set_refine_candidates(refineCandidates);
  if (exact) knn.set_exact(true);
  auto& f32 = *knn.mutable_query()->mutable_f32();
  for (float v : queryVec) f32.add_v(v);

  req->execute();
  std::vector<std::string> ids;
  if (!req->responses.empty()) {
    const auto& response = req->responses[0]->proto;
    EXPECT_TRUE(response.error().empty()) << response.error();
    auto opIt = response.ops().find("q");
    if (opIt != response.ops().end()) {
      auto idIt = opIt->second.docs().columns().find("id");
      if (idIt != opIt->second.docs().columns().end()) {
        for (const auto& id : idIt->second.col_s().v()) ids.push_back(id);
      }
    }
  }
  req->done();
  return ids;
}

} // namespace

// One segment, one field, build all aux indexes via "*".  Verify a FAISS index file
// is written with the expected ntotal / dims and that the IndexInfo references it.
TEST_F(VectorIndexBuilderTest, basicBuildSingleSegment) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  std::vector<std::vector<float>> vecs;
  vecs.reserve(80);
  for (int i = 0; i < 80; i++) {
    vecs.push_back({(float)(i % 10), (float)(i / 10), 1.0f, 0.5f});
  }
  for (size_t i = 0; i < vecs.size(); i++) {
    Doc d = flatdoc("id", "doc" + std::to_string(i), "embedding_v", vecs[i]);
    h.index(d);
  }

  // Trigger a commit that also rebuilds all aux indexes.
  h.commit({"*"});

  auto& shardDir = h.getIndexWriter()->dir;

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(shardDir, arena);
  EXPECT_EQ(0, info->aux_indexes_size());
  const auto& aux = onlyVectorOverlay(info);
  EXPECT_EQ(aux.kind(), "vector_faiss");
  EXPECT_EQ(aux.field(), "embedding_v");
  EXPECT_EQ(aux.name(), "vec.embedding_v");
  EXPECT_EQ(aux.built_core_gen(), 0u);
  ASSERT_EQ(aux.files_size(), 1);

  // Read and verify the FAISS index.
  auto idx = readFaissIndex(shardDir, aux.files(0));
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->d, 4);
  EXPECT_EQ(idx->ntotal, (faiss::idx_t)vecs.size());
  EXPECT_EQ(idx->metric_type, faiss::METRIC_L2);
}

// Empty selectors -> no aux index is built even on a populated index.
TEST_F(VectorIndexBuilderTest, noBuildWhenSelectorsEmpty) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  Doc d = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0});
  h.index(d, UpdateMessage::COMMIT);

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  EXPECT_EQ(0, info->aux_indexes_size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}

// Multiple segments - ntotal must equal the live-doc count across segments,
// and the metric file's mapping should round-trip (segId, localDoc) correctly.
TEST_F(VectorIndexBuilderTest, buildAcrossMultipleSegments) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  // Three segments, each committed (no aux build) so they end up as separate segments.
  for (int seg = 0; seg < 3; seg++) {
    for (int i = 0; i < 80; i++) {
      Doc d = flatdoc("id", "s" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)seg, (float)i, 1.0f});
      h.index(d);
    }
    h.commit();
  }

  // Final commit triggers the build over all three segments.
  h.commit({"vec.embedding_v"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  EXPECT_EQ(0, info->aux_indexes_size());
  auto overlays = vectorOverlays(info);
  ASSERT_EQ(overlays.size(), 3u);

  for (const auto* aux : overlays) {
    ASSERT_EQ(aux->files_size(), 1);
    auto idx = readFaissIndex(h.getIndexWriter()->dir, aux->files(0));
    EXPECT_EQ(idx->d, 3);
    EXPECT_EQ(idx->ntotal, 80);

    auto meta = readVectorAuxMeta(*aux);
    EXPECT_EQ(meta.dims, 3);
    EXPECT_EQ(meta.metric, (int32_t)proto::VectorParams::L2);
    EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  }
}

// Selector miss: name doesn't match any field -> no aux index built.
TEST_F(VectorIndexBuilderTest, selectorMiss) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  Doc d = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 2, 3});
  h.index(d);
  h.commit({"vec.does_not_exist_v"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  EXPECT_EQ(0, info->aux_indexes_size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}

// A delete-only second commit doesn't change segment composition (coreGen),
// so the vector aux index built for the previous commit remains valid and is
// carried forward (its files are not deleted).
TEST_F(VectorIndexBuilderTest, carryForwardOnDeleteOnlyCommit) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 1.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  const auto& orig = onlyVectorOverlay(info1);
  std::vector<std::string> origFiles(orig.files().begin(), orig.files().end());

  // Delete one doc and commit - segment composition unchanged.
  std::vector<std::string> ids{"doc0"};
  h.deleteByIds(ids, UpdateMessage::COMMIT);

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  const auto& carried = onlyVectorOverlay(info2);
  EXPECT_EQ(carried.name(), "vec.embedding_v");
  EXPECT_EQ(carried.built_core_gen(), 0u);
  EXPECT_EQ(std::vector<std::string>(carried.files().begin(), carried.files().end()), origFiles);
  // Files survive on disk.
  for (const auto& f : origFiles) {
    EXPECT_NE(h.getIndexWriter()->dir.openFile(f), nullptr) << "carried file: " << f;
  }
}

// Adding a new segment without rebuilding does not invalidate existing
// per-segment overlays.  The old segment carries its entry; the new segment
// serves flat until a build is requested and passes thresholds.
TEST_F(VectorIndexBuilderTest, segmentChangeCarriesExistingOverlay) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  const auto& orig = onlyVectorOverlay(info1);
  std::vector<std::string> origFiles(orig.files().begin(), orig.files().end());

  // Index one tiny segment and commit without rebuild.
  Doc d2 = flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0});
  h.index(d2, UpdateMessage::COMMIT);

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  auto overlays = vectorOverlays(info2);
  ASSERT_EQ(overlays.size(), 1u);
  EXPECT_EQ(std::vector<std::string>(overlays[0]->files().begin(), overlays[0]->files().end()), origFiles);
  for (const auto& f : origFiles) {
    EXPECT_NE(h.getIndexWriter()->dir.openFile(f), nullptr) << "carried file: " << f;
  }
}

TEST_F(VectorIndexBuilderTest, carryForwardBuildsOnlyNewAboveThresholdSegment) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  VectorIndexBuilder::ivfPqBuildCountForTests = 0;
  h.commit({"*"});
  EXPECT_EQ(VectorIndexBuilder::ivfPqBuildCountForTests, 1);

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  auto overlays1 = vectorOverlays(info1);
  ASSERT_EQ(overlays1.size(), 1u);
  std::string firstFile{overlays1[0]->files(0)};

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{100.0f + (float)i, 1.0f, 0.0f, 0.0f}));
  }
  VectorIndexBuilder::ivfPqBuildCountForTests = 0;
  h.commit();
  EXPECT_EQ(VectorIndexBuilder::ivfPqBuildCountForTests, 1);

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  auto overlays2 = vectorOverlays(info2);
  ASSERT_EQ(overlays2.size(), 2u);
  int carried = 0;
  int fresh = 0;
  for (const auto* overlay : overlays2) {
    ASSERT_EQ(overlay->files_size(), 1);
    if (overlay->files(0) == firstFile) {
      carried++;
    } else {
      fresh++;
    }
  }
  EXPECT_EQ(carried, 1);
  EXPECT_EQ(fresh, 1);
  EXPECT_NE(h.getIndexWriter()->dir.openFile(firstFile), nullptr);
}

TEST_F(VectorIndexBuilderTest, littleCommitDoesNoAnnWorkAndMatchesExact) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  std::string firstFile{onlyVectorOverlay(info1).files(0)};

  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 1000;
  VectorIndexBuilder::ivfPqBuildCountForTests = 0;
  h.index(flatdoc("id", std::string("tiny"),
                  "embedding_v", std::vector<float>{100.0f, 0.0f, 0.0f, 0.0f}));
  h.commit();
  EXPECT_EQ(VectorIndexBuilder::ivfPqBuildCountForTests, 0);

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  auto overlays = vectorOverlays(info2);
  ASSERT_EQ(overlays.size(), 1u);
  EXPECT_EQ(overlays[0]->files(0), firstFile);

  auto approx = runKnnIds(h.getSearchEngine(), "embedding_v",
                          {100.0f, 0.0f, 0.0f, 0.0f}, 3,
                          /*nprobe=*/2, /*refineCandidates=*/160);
  auto exact = runKnnIds(h.getSearchEngine(), "embedding_v",
                         {100.0f, 0.0f, 0.0f, 0.0f}, 3,
                         /*nprobe=*/2, /*refineCandidates=*/160, /*exact=*/true);
  ASSERT_EQ(exact.size(), 3u);
  EXPECT_EQ(exact[0], "tiny");
  EXPECT_EQ(approx, exact);
}

TEST_F(VectorIndexBuilderTest, rebuildWithoutReindex) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  std::string firstFile{onlyVectorOverlay(info1).files(0)};

  ASSERT_TRUE(h.getIndexWriter()->testDropSegmentOverlay("vec.embedding_v", 0));
  VectorIndexBuilder::ivfPqBuildCountForTests = 0;
  h.commit({"vec.embedding_v"});
  EXPECT_EQ(VectorIndexBuilder::ivfPqBuildCountForTests, 1);

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  const auto& rebuilt = onlyVectorOverlay(info2);
  ASSERT_EQ(rebuilt.files_size(), 1);
  EXPECT_NE(rebuilt.files(0), firstFile);

  auto ids = runKnnIds(h.getSearchEngine(), "embedding_v",
                       {0.0f, 0.0f, 1.0f, 0.0f}, 3,
                       /*nprobe=*/2, /*refineCandidates=*/160);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "doc0");
}

// A merge drops old segment overlays and builds one for the merged segment.
TEST_F(VectorIndexBuilderTest, mergeDropsOldOverlayFiles) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());
  h.getIndexWriter()->mergePolicy->setMergeFactor(2);

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 80; i++) {
      h.index(flatdoc("id", "s" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    h.commit({"*"});
  }

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  auto oldOverlays = vectorOverlays(info1);
  ASSERT_EQ(oldOverlays.size(), 2u);
  std::vector<std::string> oldFiles;
  for (const auto* overlay : oldOverlays) oldFiles.emplace_back(overlay->files(0));

  h.getIndexWriter()->mergeSegments();

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  ASSERT_EQ(info2->segments_size(), 1);
  const auto& aux = onlyVectorOverlay(info2);
  std::vector<std::string> newFiles(aux.files().begin(), aux.files().end());

  // Old files should be gone.
  for (const auto& f : oldFiles) {
    EXPECT_EQ(h.getIndexWriter()->dir.openFile(f), nullptr) << "old file should be deleted: " << f;
  }
  // New files should exist.
  for (const auto& f : newFiles) {
    EXPECT_NE(h.getIndexWriter()->dir.openFile(f), nullptr) << "new file should exist: " << f;
  }
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files(0));
  EXPECT_EQ(idx->ntotal, 160);
}

TEST_F(VectorIndexBuilderTest, vectorBuildCommitDoesNotWaitForInFlightMerge) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();
  iw->mergePolicy->setMergeFactor(2);

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  std::latch mergeStarted(1);
  std::latch releaseMerge(1);
  solux::Signal::listen("mergeStart", [&](void* a, void* b, void* c) -> void* {
    unused(a, b, c);
    mergeStarted.count_down();
    releaseMerge.wait();
    return nullptr;
  });

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{100.0f + (float)i, 0.0f, 0.0f, 0.0f}));
  }

  std::latch secondCommitDone(1);
  iw->commit([&]() {
    secondCommitDone.count_down();
  }, UpdateMessage::COMMIT);
  secondCommitDone.wait();
  mergeStarted.wait();

  class AsyncBuildCommit final : public UpdateMessage {
    std::atomic_bool& doneFlag;
    std::latch& doneLatch;

  public:
    AsyncBuildCommit(std::atomic_bool& doneFlag, std::latch& doneLatch)
      : doneFlag(doneFlag), doneLatch(doneLatch) {
      commit = COMMIT;
      buildAuxIndexes.push_back("vec.embedding_v");
    }

    void handle(IndexWriter& iw) override {
      unused(iw);
    }

    void done(IndexWriter& iw) override {
      unused(iw);
      doneFlag.store(true);
      doneLatch.count_down();
    }
  };

  std::atomic_bool buildDone{false};
  std::latch buildDoneLatch(1);
  AsyncBuildCommit buildCommit(buildDone, buildDoneLatch);
  ASSERT_TRUE(iw->submitUpdate(&buildCommit));

  for (int i = 0; i < 100 && !buildDone.load(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(buildDone.load()) << "vector build commit waited for blocked merge";

  auto beforeApprox = runKnnIds(h.getSearchEngine(), "embedding_v",
                                {100.0f, 0.0f, 0.0f, 0.0f}, 3,
                                /*nprobe=*/2, /*refineCandidates=*/200);
  auto beforeExact = runKnnIds(h.getSearchEngine(), "embedding_v",
                               {100.0f, 0.0f, 0.0f, 0.0f}, 3,
                               /*nprobe=*/2, /*refineCandidates=*/200, /*exact=*/true);
  EXPECT_EQ(beforeApprox, beforeExact);

  releaseMerge.count_down();
  if (!buildDone.load()) buildDoneLatch.wait();
  iw->updateGraph.wait_for_all();

  auto afterApprox = runKnnIds(h.getSearchEngine(), "embedding_v",
                               {100.0f, 0.0f, 0.0f, 0.0f}, 3,
                               /*nprobe=*/2, /*refineCandidates=*/200);
  auto afterExact = runKnnIds(h.getSearchEngine(), "embedding_v",
                              {100.0f, 0.0f, 0.0f, 0.0f}, 3,
                              /*nprobe=*/2, /*refineCandidates=*/200, /*exact=*/true);
  EXPECT_EQ(afterApprox, afterExact);
}

// Cosine metric normalizes on column write by default.  The FAISS builder can
// add the already-normalized column bytes directly as inner-product vectors.
TEST_F(VectorIndexBuilderTest, cosineNormalizeOnWriteBuildsInnerProductIndex) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();

  // Schema: _v with metric=COSINE.
  proto::SchemaDef def;
  auto* f = def.add_fields();
  f->set_name("_v");
  f->set_field_class(proto::FieldDef::VECTOR);
  f->set_abstract(true);
  f->set_column_stored(true);
  f->mutable_vector()->set_metric(proto::VectorParams::COSINE);
  h.collection().setSchema(Schema::fromProto(def, h.collection().getSchema().get()));

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "v_v", std::vector<float>{(float)(i + 1), 1.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  auto& aux = onlyVectorOverlay(info);
  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files(0));
  EXPECT_EQ(idx->metric_type, faiss::METRIC_INNER_PRODUCT);
  EXPECT_EQ(idx->ntotal, 80);
}

// normalized=true: writer and builder trust vectors are already unit-length and
// skip renormalization.
TEST_F(VectorIndexBuilderTest, normalizedFlagSkipsRenorm) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();

  proto::SchemaDef def;
  auto* f = def.add_fields();
  f->set_name("_v");
  f->set_field_class(proto::FieldDef::VECTOR);
  f->set_abstract(true);
  f->set_column_stored(true);
  f->mutable_vector()->set_metric(proto::VectorParams::COSINE);
  f->mutable_vector()->set_normalized(true);
  h.collection().setSchema(Schema::fromProto(def, h.collection().getSchema().get()));

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "v_v", std::vector<float>{2.0f, (float)(i + 1), 0.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  auto& aux = onlyVectorOverlay(info);
  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files(0));
  EXPECT_EQ(idx->ntotal, 80);
}

// Requesting a rebuild when nothing has changed (coreGen unchanged, entry
// still valid) should NOT rewrite the file - the carried-forward entry's
// existing files survive untouched.
TEST_F(VectorIndexBuilderTest, rebuildSkippedWhenStillValid) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  std::string origFile{onlyVectorOverlay(info1).files(0)};

  // Delete-only commit (coreGen unchanged) that *also* requests a rebuild via
  // "*".  The rebuild should be a no-op because the carried entry is still
  // valid; the original file should still be on disk and referenced.
  std::vector<std::string> ids{"doc0"};
  h.deleteByIds(ids);
  h.commit({"*"});

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  EXPECT_EQ(onlyVectorOverlay(info2).files(0), origFile)
      << "filename should be unchanged (no rebuild)";
  EXPECT_NE(h.getIndexWriter()->dir.openFile(origFile), nullptr);
}

// Cosine renormalization happens in fixed-size chunks so a single segment with
// many vectors doesn't blow up memory.  Set a tiny chunk size and verify the
// loop boundaries - every vector still ends up correctly normalized.
TEST_F(VectorIndexBuilderTest, cosineRenormChunkBoundaries) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  // Force a chunk size of 2 vectors x 4 floats x 4 bytes = 32 bytes per chunk.
  // With 7 vectors this exercises 4 chunks (sizes 2, 2, 2, 1).
  size_t saved = VectorIndexBuilder::renormChunkBytes;
  VectorIndexBuilder::renormChunkBytes = 32;

  CollectionHelper h("main");
  h.clear();

  proto::SchemaDef def;
  auto* f = def.add_fields();
  f->set_name("_v");
  f->set_field_class(proto::FieldDef::VECTOR);
  f->set_abstract(true);
  f->set_column_stored(true);
  f->mutable_vector()->set_metric(proto::VectorParams::COSINE);
  f->mutable_vector()->set_normalize_on_write(false);
  h.collection().setSchema(Schema::fromProto(def, h.collection().getSchema().get()));

  std::vector<std::vector<float>> vecs = {
    {3, 0, 0, 0}, {0, 5, 0, 0}, {0, 0, 7, 0}, {0, 0, 0, 9},
    {2, 2, 0, 0}, {0, 0, 4, 4}, {1, 1, 1, 1},
  };
  for (int i = 0; i < 80; i++) {
    Doc d = flatdoc("id", "doc" + std::to_string(i), "v_v", vecs[(size_t)i % vecs.size()]);
    h.index(d);
  }
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  auto& aux = onlyVectorOverlay(info);
  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 1);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files(0));
  ASSERT_EQ(idx->ntotal, 80);

  VectorIndexBuilder::renormChunkBytes = saved;
}

TEST_F(VectorIndexBuilderTest, buildsIvfPqAuxIndex) {
  IvfPqGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2, /*nprobe=*/4, /*minTraining=*/16);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 160; i++) {
    float x = (float)(i % 16);
    float y = (float)(i / 16);
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{x, y, x * 0.5f, y * 0.5f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  EXPECT_EQ(0, info->aux_indexes_size());
  const auto& aux = onlyVectorOverlay(info);
  EXPECT_EQ(aux.kind(), "vector_faiss");
  EXPECT_EQ(aux.name(), "vec.embedding_v");
  EXPECT_EQ(aux.built_core_gen(), 0u);
  ASSERT_EQ(aux.files_size(), 1);

  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files(0));
  auto* ivfpq = dynamic_cast<faiss::IndexIVFPQ*>(idx.get());
  ASSERT_NE(ivfpq, nullptr);
  EXPECT_EQ(ivfpq->d, 4);
  EXPECT_EQ(ivfpq->ntotal, 160);
  EXPECT_EQ(ivfpq->nlist, 4u);
  EXPECT_EQ(ivfpq->nprobe, 4u);
  EXPECT_EQ(ivfpq->pq.M, 2u);
  EXPECT_EQ(ivfpq->pq.nbits, 2u);

  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.engine, VectorAuxMeta::ENGINE_IVFPQ);
  EXPECT_EQ(meta.nlist, 4);
  EXPECT_EQ(meta.defaultBreadth, 4);
  EXPECT_EQ(meta.pqM, 2);
  EXPECT_EQ(meta.pqBits, 2);
}

TEST_F(VectorIndexBuilderTest, ivfPqFallsBackWhenTooSmallToTrain) {
  IvfPqGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/4, /*nprobe=*/4, /*minTraining=*/128);
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 8; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  EXPECT_EQ(0, info->aux_indexes_size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}

// Field with metric=NONE (the default _v) is not eligible for build, even via "*".
// SetUp restores the default schema, so no extra reset needed here.
TEST_F(VectorIndexBuilderTest, metricNoneIsIneligible) {
  CollectionHelper h("main");
  h.clear();

  Doc d = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 2, 3});
  h.index(d);
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  EXPECT_EQ(0, info->aux_indexes_size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}
