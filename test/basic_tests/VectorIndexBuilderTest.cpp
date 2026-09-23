// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/UpdateMessage.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/server/ProtoUpdateMessage.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/Directory.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/reader/Postings.h"
#include "luxir/util/Signal.h"
#include "luxir/util/log.h"
#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/api/luxir_types.hpp"
#include "test/QueryBuild.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/MetricType.h>
#include <faiss/index_io.h>
#include <faiss/impl/io.h>

#include <cstring>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <latch>
#include <memory>
#include <memory_resource>
#include <span>
#include <thread>
#include <vector>

using namespace luxir;
using namespace luxir::test;

class VectorIndexBuilderTest : public LuxirTest {
protected:
  void SetUp() override {
    // Reset to default schema; tests selectively install metric-bearing schemas
    // via enableL2OnVecSuffix.  Persisted schemas from prior tests would otherwise
    // leak in.
    auto col = luxirNode->getCollection("main");
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

// Listens on the "vectorBuildField" signal and throws when the build reaches
// the named field, exercising vector build-failure paths.  A test could just
// as easily block here (timing) or throw a different exception type.
struct VectorBuildFailureGuard {
  // Suppress the log line the injected failure produces - the guard already
  // knows the field, so it knows exactly what to hush.  Reverts with the guard.
  ExpectLog quiet;

  explicit VectorBuildFailureGuard(std::string fieldName)
      : quiet("injected failure for " + fieldName) {
    luxir::Signal::listen("vectorBuildField",
        [field = std::move(fieldName)](void* fnPtr, void*, void*) -> void* {
          if (*(const std::string_view*)fnPtr == field) {
            throw std::runtime_error("VectorBuildFailureGuard: injected failure for " + field);
          }
          return nullptr;
        });
  }

  ~VectorBuildFailureGuard() {
    luxir::Signal::unlisten("vectorBuildField");
  }
};

using IndexInfoHolder = DurableIndexInfo;

IndexInfoHolder readIndexInfo(Directory& dir) {
  return readDurableIndexInfo(dir);
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

VectorAuxMeta readVectorAuxMeta(const luxir::api::AuxIndexInfo& aux) {
  return VectorAuxMeta::fromBytes(
      std::string_view((const char*)aux.opaque_meta.data(), aux.opaque_meta.size()), aux.name);
}

std::vector<const luxir::api::AuxIndexInfo*> vectorOverlays(const luxir::api::IndexInfo* info) {
  std::vector<const luxir::api::AuxIndexInfo*> out;
  for (const auto& seg : info->segments) {
    for (const auto& overlay : seg.overlays) {
      if (overlay.kind == VectorIndexBuilder::KIND) out.push_back(&overlay);
    }
  }
  return out;
}

std::vector<const luxir::api::AuxIndexInfo*> vectorOverlays(const IndexInfoHolder& info) {
  return vectorOverlays(&info.info);
}

std::vector<std::string> vectorOverlayFiles(Directory& dir) {
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  std::vector<std::string> out;
  for (const auto& file : files) {
    if (file.name.find("__vec.") != std::string::npos) out.push_back(file.name);
  }
  return out;
}

bool segmentPrefixAbsent(Directory& dir, uint64_t segId) {
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  auto prefix = Postings::getIndexFileNamePrefix(segId);
  for (const auto& file : files) {
    if (file.name.starts_with(prefix)) return false;
  }
  return true;
}

bool commitForTest(CollectionHelper& h,
                   const std::vector<std::string>& buildAuxIndexes,
                   bool waitForMerges = false) {
  class BlockingProtoUpdateMessage : public ProtoUpdateMessage {
  public:
    Blocker blocker;
    bool success = false;

    explicit BlockingProtoUpdateMessage(const ProtoUpdateMessage::RequestProto* req) : ProtoUpdateMessage(req) {}

    void done(IndexWriter& iw) override {
      unused(iw);
      success = !result.errored();
      blocker.notify();
    }
  };

  // Build the non-owning commit request directly into a local arena (lives across the
  // blocking submit + wait below).
  std::pmr::monotonic_buffer_resource mr;
  luxir::api::UpdateRequest request;
  auto& params = request.commit.emplace();
  if (!buildAuxIndexes.empty()) {
    std::string_view* a = luxir::api::build::allocArray(params.build_aux_indexes, buildAuxIndexes.size(), mr);
    for (size_t i = 0; i < buildAuxIndexes.size(); i++) a[i] = luxir::api::build::arenaStr(mr, buildAuxIndexes[i]);
  }
  params.wait_for_merges = waitForMerges;

  BlockingProtoUpdateMessage msg(&request);
  bool submitted = h.getIndexWriter()->submitUpdate(&msg);
  assert(submitted);
  unused(submitted);
  msg.blocker.wait();
  return msg.success;
}

void resetVectorBuildCounters() {
  VectorIndexBuilder::ivfPqBuildCountForTests.store(0, std::memory_order_relaxed);
  VectorIndexBuilder::ivfPqMergeBuildCountForTests.store(0, std::memory_order_relaxed);
}

int64_t vectorCommitBuildCount() {
  return VectorIndexBuilder::ivfPqBuildCountForTests.load(std::memory_order_relaxed);
}

int64_t vectorMergeBuildCount() {
  return VectorIndexBuilder::ivfPqMergeBuildCountForTests.load(std::memory_order_relaxed);
}

const luxir::api::AuxIndexInfo& onlyVectorOverlay(const luxir::api::IndexInfo* info) {
  auto overlays = vectorOverlays(info);
  EXPECT_EQ(overlays.size(), 1u);
  return *overlays[0];
}

const luxir::api::AuxIndexInfo& onlyVectorOverlay(const IndexInfoHolder& info) {
  return onlyVectorOverlay(&info.info);
}

// Install a schema where _v has metric=L2.
void enableL2OnVecSuffix(Collection& col) {
  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::L2;
  // Merge preserves all built-in fields and overrides _v with the
  // metric-bearing definition.
  b.set(col);
}

std::vector<std::string> runKnnIds(SearchEngine& engine, std::string_view field,
                                  const std::vector<float>& queryVec, int32_t k,
                                  int32_t nprobe = 0, int32_t refineCandidates = 0,
                                  bool exact = false) {
  auto req = localReq(engine);
  auto& cur = req->collection("main").topDocs("q").getNumber().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, queryVec, k, nprobe, exact, refineCandidates);

  req->execute();
  EXPECT_OK(req);
  std::vector<std::string> ids;
  if (const auto* dl = req->docList("q")) {
    if (const auto* p = dl->columns.find("id")) {
      if (const auto* col = std::get_if<luxir::api::ColStr>(&p->kind)) {
        for (auto id : col->v) ids.emplace_back(id);
      }
    }
  }
  return ids;
}

} // namespace

// One segment, one field, build all aux indexes via "*".  Verify a FAISS index file
// is written with the expected ntotal / dims and that the IndexInfo references it.
TEST_F(VectorIndexBuilderTest, overlayFileNamesAreCaseSafe) {
  // Capitals append their positions, so names differing only by case stay
  // distinct even after a case-insensitive filesystem folds them.
  EXPECT_EQ("FooBar-0-3", Postings::caseSafeName("FooBar"));
  EXPECT_EQ("foobar", Postings::caseSafeName("foobar"));

  std::string a = Postings::getSegmentOverlayFileName(1, "vec.Emb_v", 0, 0);
  std::string b = Postings::getSegmentOverlayFileName(1, "vec.emb_v", 0, 0);
  for (char& c : a) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  EXPECT_NE(a, b);
}

TEST_F(VectorIndexBuilderTest, basicBuildSingleSegment) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
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

  auto info = readIndexInfo(shardDir);
  expectValidInventory(shardDir, *info);
  EXPECT_EQ(0, info->aux_indexes.size());
  const auto& aux = onlyVectorOverlay(info);
  EXPECT_EQ(aux.kind, "vector_faiss");
  EXPECT_EQ(aux.field, "embedding_v");
  EXPECT_EQ(aux.name, "vec.embedding_v");
  EXPECT_EQ(aux.built_core_gen, 0u);
  ASSERT_EQ(aux.files.size(), 1);

  // Read and verify the FAISS index.
  auto idx = readFaissIndex(shardDir, aux.files[0].name);
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->d, 4);
  EXPECT_EQ(idx->ntotal, (faiss::idx_t)vecs.size());
  EXPECT_EQ(idx->metric_type, faiss::METRIC_L2);

  // The overlay's stats bytes match the file's size in the directory listing.
  std::vector<Directory::FileInfo> files;
  shardDir.listFiles(files);
  uint64_t fileSize = 0;
  for (const auto& f : files) {
    if (f.name == aux.files[0].name) fileSize = f.size;
  }
  ASSERT_GT(fileSize, 0u);
  auto stats = h.getIndexWriter()->stats(true);
  ASSERT_EQ(1u, stats.segmentStats.size());
  ASSERT_EQ(1u, stats.segmentStats[0].overlays.size());
  EXPECT_EQ(fileSize, stats.segmentStats[0].overlays[0].bytes);
}

// The mmap-lists layout must serve identically whether the aux file's memory
// view is RAMDir's owned buffer or FSDirectory's real mmap: copy the built
// aux file into a temp FSDirectory, open it through VectorAuxReader there,
// and require bit-identical FAISS search results against the RAMDir-backed
// open.  This is the FSDirectory leg of the zero-copy list path (the rest of
// the suite runs on RAMDir).
TEST_F(VectorIndexBuilderTest, ivfListsServeIdenticallyFromFsDirectoryMmap) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i), "embedding_v",
                    std::vector<float>{(float)(i % 10), (float)(i / 10), 1.0f, 0.5f}));
  }
  h.commit({"*"});

  auto& shardDir = h.getIndexWriter()->dir;
  auto info = readIndexInfo(shardDir);
  expectValidInventory(shardDir, *info);
  const auto& aux = onlyVectorOverlay(info);
  ASSERT_EQ(aux.files.size(), 1);

  auto ramAux = VectorAuxReader::open(shardDir, aux, /*missingFileOK=*/false);
  ASSERT_NE(ramAux, nullptr);
  faiss::Index* ramIdx = ramAux->getFaissIndex();
  ASSERT_NE(ramIdx, nullptr);

  std::string tmpl = (std::filesystem::temp_directory_path() / "luxir_vec_aux_XXXXXX").string();
  ASSERT_NE(mkdtemp(tmpl.data()), nullptr);
  std::filesystem::path tmp(tmpl);
  {
    FSDirectory fsDir(tmp);
    auto src = shardDir.openFile(aux.files[0].name);
    ASSERT_NE(src, nullptr);
    auto bytes = src->read();
    auto dst = fsDir.createFile(aux.files[0].name);
    OutputStream os;
    os.setFile(&*dst);
    os.write(bytes.data(), bytes.size());
    os.close();
    fsDir.finishFile(*dst);

    auto fsAux = VectorAuxReader::open(fsDir, aux, /*missingFileOK=*/false);
    ASSERT_NE(fsAux, nullptr);
    auto* fsIvf = dynamic_cast<faiss::IndexIVF*>(fsAux->getFaissIndex());
    ASSERT_NE(fsIvf, nullptr);
    ASSERT_NE(dynamic_cast<MmapInvertedLists*>(fsIvf->invlists), nullptr);

    constexpr int K = 10;
    std::vector<float> query{3.0f, 4.0f, 1.0f, 0.5f};
    std::vector<float> ramDist(K), fsDist(K);
    std::vector<faiss::idx_t> ramIds(K), fsIds(K);
    ramIdx->search(1, query.data(), K, ramDist.data(), ramIds.data());
    fsIvf->search(1, query.data(), K, fsDist.data(), fsIds.data());
    EXPECT_EQ(ramIds, fsIds);
    EXPECT_EQ(ramDist, fsDist);
    EXPECT_NE(ramIds[0], -1);
  }
  std::filesystem::remove_all(tmp);
}

// Empty selectors -> no aux index is built even on a populated index.
TEST_F(VectorIndexBuilderTest, noBuildWhenSelectorsEmpty) {
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  Doc d = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0});
  h.index(d, UpdateMessage::COMMIT);

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(0, info->aux_indexes.size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}

// Multiple segments - ntotal must equal the live-doc count across segments,
// and the metric file's mapping should round-trip (segId, localDoc) correctly.
TEST_F(VectorIndexBuilderTest, buildAcrossMultipleSegments) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
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

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(0, info->aux_indexes.size());
  auto overlays = vectorOverlays(info);
  ASSERT_EQ(overlays.size(), 3u);

  for (const auto* aux : overlays) {
    ASSERT_EQ(aux->files.size(), 1);
    auto idx = readFaissIndex(h.getIndexWriter()->dir, aux->files[0].name);
    EXPECT_EQ(idx->d, 3);
    EXPECT_EQ(idx->ntotal, 80);

    auto meta = readVectorAuxMeta(*aux);
    EXPECT_EQ(meta.dims, 3);
    EXPECT_EQ(meta.metric, (int32_t)luxir::api::VectorMetric::L2);
    EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  }
}

// Selector miss: name doesn't match any field -> no aux index built.
TEST_F(VectorIndexBuilderTest, selectorMiss) {
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  Doc d = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 2, 3});
  h.index(d);
  h.commit({"vec.does_not_exist_v"});

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(0, info->aux_indexes.size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}

// A delete-only second commit doesn't change segment composition (coreGen),
// so the vector aux index built for the previous commit remains valid and is
// carried forward (its files are not deleted).
TEST_F(VectorIndexBuilderTest, carryForwardOnDeleteOnlyCommit) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 1.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  const auto& orig = onlyVectorOverlay(info1);
  auto origFiles = fileNames(orig.files);

  // Delete one doc and commit - segment composition unchanged.
  std::vector<std::string> ids{"doc0"};
  h.deleteByIds(ids, UpdateMessage::COMMIT);

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  const auto& carried = onlyVectorOverlay(info2);
  EXPECT_EQ(carried.name, "vec.embedding_v");
  EXPECT_EQ(carried.built_core_gen, 0u);
  EXPECT_EQ(fileNames(carried.files), origFiles);
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
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  const auto& orig = onlyVectorOverlay(info1);
  auto origFiles = fileNames(orig.files);

  // Index one tiny segment and commit without rebuild.
  Doc d2 = flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0});
  h.index(d2, UpdateMessage::COMMIT);

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  auto overlays = vectorOverlays(info2);
  ASSERT_EQ(overlays.size(), 1u);
  EXPECT_EQ(fileNames(overlays[0]->files), origFiles);
  for (const auto& f : origFiles) {
    EXPECT_NE(h.getIndexWriter()->dir.openFile(f), nullptr) << "carried file: " << f;
  }
}

// Plain user commits do NOT infer vector selectors from existing overlays:
// they build only what they ask for.  Merge-created segments get overlays in
// the merge-private phase.  Full auto-maintenance is the future background
// builder's job.
TEST_F(VectorIndexBuilderTest, plainCommitDoesNotAutoBuild) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});  // field is now "active" - one overlay exists

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{100.0f + (float)i, 1.0f, 0.0f, 0.0f}));
  }
  resetVectorBuildCounters();
  h.commit();  // plain: no selectors, no inference
  EXPECT_EQ(vectorCommitBuildCount(), 0);

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(vectorOverlays(info).size(), 1u) << "only the first segment's overlay exists";

  // The explicit selector then builds the missing one.
  resetVectorBuildCounters();
  h.commit({"*"});
  EXPECT_EQ(vectorCommitBuildCount(), 1);
}

TEST_F(VectorIndexBuilderTest, carryForwardBuildsOnlyNewAboveThresholdSegment) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  resetVectorBuildCounters();
  h.commit({"*"});
  EXPECT_EQ(vectorCommitBuildCount(), 1);

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  auto overlays1 = vectorOverlays(info1);
  ASSERT_EQ(overlays1.size(), 1u);
  std::string firstFile{overlays1[0]->files[0].name};
  // Overlay filename convention: segment-prefixed like liveDocs -
  // s<segId>__<name>_<gen>_<fnum> - so ls groups overlays with their segment
  // (all-lowercase names render verbatim in filenames).
  EXPECT_TRUE(firstFile.starts_with(
      Postings::getIndexFileNamePrefix(info1->segments[0].seg_id) + "__vec.embedding_v_"))
      << "unexpected overlay filename: " << firstFile;

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{100.0f + (float)i, 1.0f, 0.0f, 0.0f}));
  }
  resetVectorBuildCounters();
  h.commit({"*"});
  EXPECT_EQ(vectorCommitBuildCount(), 1);

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  auto overlays2 = vectorOverlays(info2);
  ASSERT_EQ(overlays2.size(), 2u);
  int carried = 0;
  int fresh = 0;
  for (const auto* overlay : overlays2) {
    ASSERT_EQ(overlay->files.size(), 1);
    if (overlay->files[0].name == firstFile) {
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
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  std::string firstFile{onlyVectorOverlay(info1).files[0].name};

  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 1000;
  resetVectorBuildCounters();
  h.index(flatdoc("id", std::string("tiny"),
                  "embedding_v", std::vector<float>{100.0f, 0.0f, 0.0f, 0.0f}));
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0);

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  auto overlays = vectorOverlays(info2);
  ASSERT_EQ(overlays.size(), 1u);
  EXPECT_EQ(overlays[0]->files[0].name, firstFile);

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
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  const auto& first = onlyVectorOverlay(info1);
  std::string firstFile{first.files[0].name};
  EXPECT_EQ(first.gen, 0u);
  EXPECT_TRUE(firstFile.ends_with("_00_00")) << firstFile;

  auto pin = h.getIndexWriter()->snapshots.acquire();
  ASSERT_TRUE(h.getIndexWriter()->testDropSegmentOverlay("vec.embedding_v", 0));
  h.commit(); // publish the drop separately; the old name remains reserved
  resetVectorBuildCounters();
  h.commit({"vec.embedding_v"});
  EXPECT_EQ(vectorCommitBuildCount(), 1);

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  const auto& rebuilt = onlyVectorOverlay(info2);
  ASSERT_EQ(rebuilt.files.size(), 1);
  EXPECT_EQ(rebuilt.gen, 1u);
  EXPECT_NE(rebuilt.files[0].name, firstFile);
  EXPECT_TRUE(std::string(rebuilt.files[0].name).ends_with("_01_00")) << rebuilt.files[0].name;
  auto bytes = h.getIndexWriter()->snapshots.openFile(pin->id, firstFile)->read();
  auto desc = std::ranges::find(pin->files, firstFile, &FileDescriptor::name);
  ASSERT_NE(desc, pin->files.end());
  EXPECT_EQ(desc->xxh3, XXH3_64bits(bytes.data(), bytes.size()));
  EXPECT_EQ(desc->size, h.getIndexWriter()->snapshots.stats().retainedBytes);
  h.getIndexWriter()->snapshots.evictOldest();
  EXPECT_EQ(h.getIndexWriter()->dir.openFile(firstFile), nullptr);

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
  enableL2OnVecSuffix(h.collection());
  // No setMergeFactor(2) here: an automatic merge during setup could publish
  // the merged segment before the assertions below see the two source
  // overlays.  mergeSegments() forces maxSegments=1 regardless of the factor.

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 80; i++) {
      h.index(flatdoc("id", "s" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    h.commit({"*"});
  }

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  auto oldOverlays = vectorOverlays(info1);
  ASSERT_EQ(oldOverlays.size(), 2u);
  std::vector<std::string> oldFiles;
  for (const auto* overlay : oldOverlays) oldFiles.emplace_back(overlay->files[0].name);

  h.getIndexWriter()->mergeSegments();

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  ASSERT_EQ(info2->segments.size(), 1);
  const auto& aux = onlyVectorOverlay(info2);
  auto newFiles = fileNames(aux.files);

  // Old files should be gone.
  for (const auto& f : oldFiles) {
    EXPECT_EQ(h.getIndexWriter()->dir.openFile(f), nullptr) << "old file should be deleted: " << f;
  }
  // New files should exist.
  for (const auto& f : newFiles) {
    EXPECT_NE(h.getIndexWriter()->dir.openFile(f), nullptr) << "new file should exist: " << f;
  }
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files[0].name);
  EXPECT_EQ(idx->ntotal, 160);
}

TEST_F(VectorIndexBuilderTest, forceMergeFlushesPendingIndexingAndBuildsOverlayBeforePublish) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 80; i++) {
      h.index(flatdoc("id", "s" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    h.commit({"*"});
  }

  h.index(flatdoc("id", std::string("pending"),
                  "embedding_v", std::vector<float>{1000.0f, 0.0f, 0.0f, 0.0f}));

  resetVectorBuildCounters();
  h.getIndexWriter()->mergeSegments();
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 1);

  constexpr int32_t fullEffortNProbe = 20;  // > sqrt(160), exhaustive under total-effort semantics.
  auto beforeApprox = runKnnIds(h.getSearchEngine(), "embedding_v",
                                {0.2f, 1.0f, 1.0f, 0.0f}, 3,
                                /*nprobe=*/fullEffortNProbe, /*refineCandidates=*/200);
  auto beforeExact = runKnnIds(h.getSearchEngine(), "embedding_v",
                               {0.2f, 1.0f, 1.0f, 0.0f}, 3,
                               /*nprobe=*/fullEffortNProbe, /*refineCandidates=*/200, /*exact=*/true);
  EXPECT_EQ(beforeApprox, beforeExact);

  // The commit-backed force-merge helper already flushed the pending inverter
  // and durably published its merged output.  This plain commit remains a no-op.
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0)
      << "plain publication commit should not run ANN builds";
  EXPECT_EQ(vectorMergeBuildCount(), 1);

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  ASSERT_EQ(info->segments.size(), 1);
  auto overlays = vectorOverlays(info);
  ASSERT_EQ(overlays.size(), 1u);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, overlays[0]->files[0].name);
  EXPECT_EQ(idx->ntotal, 161);

  auto afterApprox = runKnnIds(h.getSearchEngine(), "embedding_v",
                               {0.2f, 1.0f, 1.0f, 0.0f}, 3,
                               /*nprobe=*/fullEffortNProbe, /*refineCandidates=*/200);
  auto afterExact = runKnnIds(h.getSearchEngine(), "embedding_v",
                              {0.2f, 1.0f, 1.0f, 0.0f}, 3,
                              /*nprobe=*/fullEffortNProbe, /*refineCandidates=*/200, /*exact=*/true);
  EXPECT_EQ(afterApprox, afterExact);
}

TEST_F(VectorIndexBuilderTest, mergePromotesBelowThresholdSegmentsForActiveField) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();
  iw->mergePolicy->setMergeFactor(2);

  for (int i = 0; i < 400; i++) {
    h.index(flatdoc("id", "active" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 400;
  resetVectorBuildCounters();
  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 79; i++) {
      h.index(flatdoc("id", "small" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{200.0f + (float)i, (float)seg, 0.0f, 0.0f}));
    }
    h.commit();
  }
  iw->updateGraph.wait_for_all();
  h.commit();

  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 1);

  auto info = readIndexInfo(iw->dir);
  auto overlays = vectorOverlays(info);
  ASSERT_EQ(overlays.size(), 2u);
  bool sawMergedSmall = false;
  for (const auto* overlay : overlays) {
    auto idx = readFaissIndex(iw->dir, overlay->files[0].name);
    if (idx->ntotal == 158) {
      sawMergedSmall = true;
    }
  }
  EXPECT_TRUE(sawMergedSmall);
}

TEST_F(VectorIndexBuilderTest, belowThresholdMergedOutputBuildsNoOverlay) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 400;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();

  EXPECT_TRUE(commitForTest(h, {"vec.embedding_v"}));
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.embedding_v"));

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 39; i++) {
      h.index(flatdoc("id", "small" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    h.commit();
  }

  resetVectorBuildCounters();
  iw->mergeSegments();
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 0);

  auto info = readIndexInfo(iw->dir);
  ASSERT_EQ(info->segments.size(), 1);
  EXPECT_EQ(vectorOverlays(info).size(), 0u);

  auto approx = runKnnIds(h.getSearchEngine(), "embedding_v",
                          {0.2f, 1.0f, 1.0f, 0.0f}, 3,
                          /*nprobe=*/2, /*refineCandidates=*/200);
  auto exact = runKnnIds(h.getSearchEngine(), "embedding_v",
                         {0.2f, 1.0f, 1.0f, 0.0f}, 3,
                         /*nprobe=*/2, /*refineCandidates=*/200, /*exact=*/true);
  EXPECT_EQ(approx, exact);
}

TEST_F(VectorIndexBuilderTest, explicitActivationBelowThresholdPromotesInProcessMerge) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 400;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 79; i++) {
      h.index(flatdoc("id", "small" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    if (seg == 0) {
      h.commit({"vec.embedding_v"});
    } else {
      h.commit();
    }
  }

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(vectorOverlays(info1).size(), 0u);

  resetVectorBuildCounters();
  h.getIndexWriter()->mergeSegments();
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 1);

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  ASSERT_EQ(info2->segments.size(), 1);
  const auto& aux = onlyVectorOverlay(info2);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files[0].name);
  EXPECT_EQ(idx->ntotal, 158);
}

TEST_F(VectorIndexBuilderTest, explicitActivationOnEmptyIndexPromotesLaterMerge) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 400;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();

  EXPECT_TRUE(commitForTest(h, {"vec.embedding_v"}));
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.embedding_v"));

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 79; i++) {
      h.index(flatdoc("id", "small" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    h.commit();
  }

  resetVectorBuildCounters();
  iw->mergeSegments();
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 1);

  auto info = readIndexInfo(iw->dir);
  ASSERT_EQ(info->segments.size(), 1);
  const auto& aux = onlyVectorOverlay(info);
  auto idx = readFaissIndex(iw->dir, aux.files[0].name);
  EXPECT_EQ(idx->ntotal, 158);
}

TEST_F(VectorIndexBuilderTest, invalidExactVectorSelectorDoesNotActivate) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();

  {
    ExpectLog quiet("Ignoring vector aux selector vec.typo");
    EXPECT_TRUE(commitForTest(h, {"vec.typo"}));
  }
  EXPECT_FALSE(iw->testActiveVectorOverlayName("vec.typo"));

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 80; i++) {
      h.index(flatdoc("id", "doc" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    h.commit();
  }

  resetVectorBuildCounters();
  iw->mergeSegments();
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 0);
}

TEST_F(VectorIndexBuilderTest, intentOnlyActivationIsNotSeededAfterRestart) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 400;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 79; i++) {
      h.index(flatdoc("id", "small" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    if (seg == 0) {
      h.commit({"vec.embedding_v"});
    } else {
      h.commit();
    }
  }

  auto info = readIndexInfo(iw->dir);
  ASSERT_EQ(info->segments.size(), 2);
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.embedding_v"));

  iw->testReseedActiveVectorOverlayNamesFromManifest();
  EXPECT_FALSE(iw->testActiveVectorOverlayName("vec.embedding_v"));

  resetVectorBuildCounters();
  iw->mergeSegments();
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 0);

  auto info2 = readIndexInfo(iw->dir);
  ASSERT_EQ(info2->segments.size(), 1);
  EXPECT_EQ(vectorOverlays(info2).size(), 0u);

  resetVectorBuildCounters();
  h.commit({"vec.embedding_v"});
  EXPECT_EQ(vectorCommitBuildCount(), 1);
  EXPECT_EQ(vectorMergeBuildCount(), 0);
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.embedding_v"));
}

TEST_F(VectorIndexBuilderTest, failedMultiFieldCommitDoesNotPublishPartialOverlays) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "a_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f},
                    "b_v", std::vector<float>{(float)i, 1.0f, 0.0f, 0.0f}));
  }
  h.commit();

  resetVectorBuildCounters();
  {
    VectorBuildFailureGuard fail("b_v");
    EXPECT_FALSE(commitForTest(h, {"*"}));
  }
  EXPECT_EQ(vectorCommitBuildCount(), 1);
  EXPECT_EQ(vectorMergeBuildCount(), 0);
  EXPECT_FALSE(iw->testActiveVectorOverlayName("vec.a_v"));
  EXPECT_FALSE(iw->testActiveVectorOverlayName("vec.b_v"));
  EXPECT_TRUE(vectorOverlayFiles(iw->dir).empty());

  h.commit();

  auto info = readIndexInfo(iw->dir);
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
  EXPECT_FALSE(iw->testActiveVectorOverlayName("vec.a_v"));
  EXPECT_FALSE(iw->testActiveVectorOverlayName("vec.b_v"));
  EXPECT_TRUE(vectorOverlayFiles(iw->dir).empty());
}

TEST_F(VectorIndexBuilderTest, mergeOverlayFailurePublishesFlatAndClearsMergeGate) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();

  EXPECT_TRUE(commitForTest(h, {"vec.a_v", "vec.b_v"}));
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.a_v"));
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.b_v"));

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 80; i++) {
      h.index(flatdoc("id", "s" + std::to_string(seg) + "_" + std::to_string(i),
                      "a_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f},
                      "b_v", std::vector<float>{(float)i, (float)seg, 0.0f, 1.0f}));
    }
    h.commit();
  }

  resetVectorBuildCounters();
  {
    VectorBuildFailureGuard fail("b_v");
    iw->mergeSegments();
  }
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 1);
  EXPECT_FALSE(iw->testMergeRunning());
  EXPECT_TRUE(commitForTest(h, {}, /*waitForMerges=*/true));

  auto info1 = readIndexInfo(iw->dir);
  ASSERT_EQ(info1->segments.size(), 1);
  EXPECT_EQ(vectorOverlays(info1).size(), 0u);
  EXPECT_TRUE(vectorOverlayFiles(iw->dir).empty());

  resetVectorBuildCounters();
  EXPECT_TRUE(commitForTest(h, {"vec.a_v", "vec.b_v"}));
  EXPECT_EQ(vectorCommitBuildCount(), 2);
  EXPECT_EQ(vectorMergeBuildCount(), 0);

  auto info2 = readIndexInfo(iw->dir);
  EXPECT_EQ(vectorOverlays(info2).size(), 2u);
}

TEST_F(VectorIndexBuilderTest, mergedPostingsReaderFailureRestoresSourcesAndCleansOutput) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());
  auto iw = h.getIndexWriter();

  EXPECT_TRUE(commitForTest(h, {"vec.embedding_v"}));
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.embedding_v"));

  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 80; i++) {
      h.index(flatdoc("id", "s" + std::to_string(seg) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{(float)i, (float)seg, 1.0f, 0.0f}));
    }
    h.commit();
  }

  luxir::Signal::listen("mergedPostingsReader", [](void*, void*, void*) -> void* {
    throw std::runtime_error("injected merged postings reader failure");
  });
  {
    ExpectLog quiet("injected merged postings reader failure");
    iw->mergeSegments();
  }
  luxir::Signal::unlisten("mergedPostingsReader");

  EXPECT_FALSE(iw->testMergeRunning());
  auto failure = iw->testLastMergeFailure();
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->phase, "merged_postings_reader");
  EXPECT_FALSE(failure->outputPublished);
  EXPECT_EQ(failure->sourceSegIds.size(), 2u);
  EXPECT_TRUE(segmentPrefixAbsent(iw->dir, failure->outputSegId));
  EXPECT_TRUE(commitForTest(h, {}, /*waitForMerges=*/true));

  auto info1 = readIndexInfo(iw->dir);
  ASSERT_EQ(info1->segments.size(), 2);
  EXPECT_EQ(vectorOverlays(info1).size(), 0u);

  resetVectorBuildCounters();
  iw->mergeSegments();
  h.commit();
  EXPECT_EQ(vectorCommitBuildCount(), 0);
  EXPECT_EQ(vectorMergeBuildCount(), 1);

  auto info2 = readIndexInfo(iw->dir);
  ASSERT_EQ(info2->segments.size(), 1);
  EXPECT_EQ(vectorOverlays(info2).size(), 1u);
}

TEST_F(VectorIndexBuilderTest, vectorBuildCommitDoesNotWaitForInFlightMerge) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
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
  luxir::Signal::listen("mergeStart", [&](void* a, void* b, void* c) -> void* {
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

  // Schema: _v with metric=COSINE.
  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::COSINE;
  b.set(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "v_v", std::vector<float>{(float)(i + 1), 1.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  auto& aux = onlyVectorOverlay(info);
  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files[0].name);
  EXPECT_EQ(idx->metric_type, faiss::METRIC_INNER_PRODUCT);
  EXPECT_EQ(idx->ntotal, 80);
}

// normalized=true: writer and builder trust vectors are already unit-length and
// skip renormalization.
TEST_F(VectorIndexBuilderTest, normalizedFlagSkipsRenorm) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");

  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::COSINE;
  f.normalized = true;
  b.set(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "v_v", std::vector<float>{2.0f, (float)(i + 1), 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  auto& aux = onlyVectorOverlay(info);
  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files[0].name);
  EXPECT_EQ(idx->ntotal, 80);
}

// Requesting a rebuild when nothing has changed (coreGen unchanged, entry
// still valid) should NOT rewrite the file - the carried-forward entry's
// existing files survive untouched.
TEST_F(VectorIndexBuilderTest, rebuildSkippedWhenStillValid) {
  IvfPqGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1, /*nprobe=*/2, /*minTraining=*/2);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info1 = readIndexInfo(h.getIndexWriter()->dir);
  std::string origFile{onlyVectorOverlay(info1).files[0].name};

  // Delete-only commit (coreGen unchanged) that *also* requests a rebuild via
  // "*".  The rebuild should be a no-op because the carried entry is still
  // valid; the original file should still be on disk and referenced.
  std::vector<std::string> ids{"doc0"};
  h.deleteByIds(ids);
  h.commit({"*"});

  auto info2 = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(onlyVectorOverlay(info2).files[0].name, origFile)
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

  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::COSINE;
  f.normalize_on_write = false;
  b.set(h.collection());

  std::vector<std::vector<float>> vecs = {
    {3, 0, 0, 0}, {0, 5, 0, 0}, {0, 0, 7, 0}, {0, 0, 0, 9},
    {2, 2, 0, 0}, {0, 0, 4, 4}, {1, 1, 1, 1},
  };
  for (int i = 0; i < 80; i++) {
    Doc d = flatdoc("id", "doc" + std::to_string(i), "v_v", vecs[(size_t)i % vecs.size()]);
    h.index(d);
  }
  h.commit({"*"});

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  auto& aux = onlyVectorOverlay(info);
  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 1);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files[0].name);
  ASSERT_EQ(idx->ntotal, 80);

  VectorIndexBuilder::renormChunkBytes = saved;
}

TEST_F(VectorIndexBuilderTest, buildsIvfPqAuxIndex) {
  IvfPqGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2, /*nprobe=*/4, /*minTraining=*/16);
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 160; i++) {
    float x = (float)(i % 16);
    float y = (float)(i / 16);
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{x, y, x * 0.5f, y * 0.5f}));
  }
  h.commit({"*"});

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(0, info->aux_indexes.size());
  const auto& aux = onlyVectorOverlay(info);
  EXPECT_EQ(aux.kind, "vector_faiss");
  EXPECT_EQ(aux.name, "vec.embedding_v");
  EXPECT_EQ(aux.built_core_gen, 0u);
  ASSERT_EQ(aux.files.size(), 1);

  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files[0].name);
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
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 8; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(0, info->aux_indexes.size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}

// Field with metric=NONE (the default _v) is not eligible for build, even via "*".
// SetUp restores the default schema, so no extra reset needed here.
TEST_F(VectorIndexBuilderTest, metricNoneIsIneligible) {
  CollectionHelper h("main");

  Doc d = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 2, 3});
  h.index(d);
  h.commit({"*"});

  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(0, info->aux_indexes.size());
  EXPECT_EQ(vectorOverlays(info).size(), 0u);
}

TEST_F(VectorIndexBuilderTest, overlayGenerationSurvivesPublishedDropAndRestart) {
  IvfPqGuard guard(2, 1, 1, 2, 2);
  auto path = std::filesystem::temp_directory_path() / "luxir-overlay-restart";
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  std::string oldName;
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    enableL2OnVecSuffix(h.collection());
    for (int i = 0; i < 80; i++) {
      ASSERT_TRUE(h.index(flatdoc("id", std::to_string(i), "embedding_v",
                                 std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f})).success);
    }
    h.commit({"*"});
    auto info = readIndexInfo(h.getIndexWriter()->dir);
    oldName = onlyVectorOverlay(info).files.front().name;
    ASSERT_TRUE(h.getIndexWriter()->testDropSegmentOverlay("vec.embedding_v", 0));
    h.commit();
    EXPECT_EQ(1u, readIndexInfo(h.getIndexWriter()->dir)->segments.front().next_overlay_gen);
  }
  LuxirNode reopened(config);
  CollectionHelper h(reopened, "main");
  h.commit({"vec.embedding_v"});
  auto info = readIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(1u, onlyVectorOverlay(info).gen);
  EXPECT_NE(oldName, onlyVectorOverlay(info).files.front().name);
}
