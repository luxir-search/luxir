#include <gtest/gtest.h>

#include <faiss/Index.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/MetricType.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/reader/AuxReader.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/LuxirNode.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class IndexReaderAuxTest : public LuxirTest {
public:
  IndexReaderAuxTest() { TestOverlayAuxReader::enabledForTests = true; }

protected:
  void SetUp() override {
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

    IvfPqGuard()
      : savedIvfPq(VectorIndexBuilder::buildFaissIvfPqAuxIndexes),
        savedNList(VectorIndexBuilder::ivfPqNList),
        savedM(VectorIndexBuilder::ivfPqM),
        savedBits(VectorIndexBuilder::ivfPqBits),
        savedNProbe(VectorIndexBuilder::ivfPqDefaultNProbe),
        savedMinTraining(VectorIndexBuilder::ivfPqMinTrainingVectors),
        savedBuildThreshold(VectorIndexBuilder::ivfPqBuildThresholdScanCost) {
      VectorIndexBuilder::buildFaissIvfPqAuxIndexes = true;
      VectorIndexBuilder::ivfPqNList = 2;
      VectorIndexBuilder::ivfPqM = 1;
      VectorIndexBuilder::ivfPqBits = 1;
      VectorIndexBuilder::ivfPqDefaultNProbe = 2;
      VectorIndexBuilder::ivfPqMinTrainingVectors = 2;
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

  // Install a schema where _v has metric=L2 so the suffix-rule fields
  // (e.g. "embedding_v") become eligible for FAISS aux indexing.
  static void enableL2OnVecSuffix(Collection& col) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
    f.column = true;
    f.metric = luxir::api::VectorMetric::L2;
    b.set(col);
  }

  static void enableCosineOnVecSuffix(Collection& col, bool normalizeOnWrite) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
    f.column = true;
    f.metric = luxir::api::VectorMetric::COSINE;
    f.normalize_on_write = normalizeOnWrite;
    b.set(col);
  }
};

namespace {

std::vector<const luxir::api::AuxIndexInfo*> vectorOverlays(const luxir::api::IndexInfo& info) {
  std::vector<const luxir::api::AuxIndexInfo*> out;
  for (const auto& seg : info.segments) {
    for (const auto& overlay : seg.overlays) {
      if (overlay.kind == VectorIndexBuilder::KIND) out.push_back(&overlay);
    }
  }
  return out;
}

// Registry split: per-segment overlays are looked up via Segment::getAuxReader,
// never via the index-level registry.  Returns the first segment's reader
// holding the named overlay (nullptr if none).
std::shared_ptr<AuxReader> firstSegmentAux(IndexReader& reader, std::string_view name) {
  for (auto& seg : reader.segments()) {
    if (auto aux = seg.getAuxReader(name)) return aux;
  }
  return nullptr;
}

const luxir::api::AuxIndexInfo& onlyVectorOverlay(const luxir::api::IndexInfo& info) {
  auto overlays = vectorOverlays(info);
  EXPECT_EQ(overlays.size(), 1u);
  return *overlays[0];
}

} // namespace

// Minimal happy path: build an aux index, open an IndexReader, and verify the
// AuxReader is present, dispatched to VectorAuxReader, and the FAISS index
// round-trips through deserialize_index correctly.
TEST_F(IndexReaderAuxTest, opensVectorAuxAfterBuild) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  std::vector<std::vector<float>> vecs;
  vecs.reserve(80);
  for (int i = 0; i < 80; i++) {
    vecs.push_back({(float)i, 1.0f, 0.0f, 0.0f});
  }
  for (size_t i = 0; i < vecs.size(); i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i), "embedding_v", vecs[i]));
  }
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;
  auto reader = std::make_shared<IndexReader>(dir);

  // Registry split guard: segment overlays are NOT in the index-level
  // registry (their names repeat per segment); index-level lookup says null.
  EXPECT_EQ(reader->auxReaders().size(), 0u);
  EXPECT_EQ(reader->getAuxReader("vec.embedding_v"), nullptr);
  auto aux = firstSegmentAux(*reader, "vec.embedding_v");
  ASSERT_NE(aux, nullptr);
  EXPECT_EQ(aux->getKind(), VectorAuxReader::KIND);

  auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
  ASSERT_NE(vaux, nullptr);
  EXPECT_EQ(vaux->getField(), "embedding_v");
  EXPECT_EQ(vaux->getDims(), 4);
  EXPECT_EQ(vaux->getMetric(), (int32_t)luxir::api::VectorMetric::L2);
  EXPECT_FALSE(vaux->shouldNormalizeColumnOnCosineRescore());

  auto* idx = vaux->getFaissIndex();
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->d, 4);
  EXPECT_EQ(idx->ntotal, (faiss::idx_t)vecs.size());
  EXPECT_EQ(idx->metric_type, faiss::METRIC_L2);
}

// mmap residency: only the IVF header (coarse quantizer + PQ codebooks) is
// deserialized into RAM; the list payloads are served zero-copy from the aux
// file's memory view through MmapInvertedLists.  Guards against silently
// falling back to a RAM-resident ArrayInvertedLists, and verifies the list
// payloads written in Luxir's own layout round-trip (every segment-local
// valueRank appears exactly once across the lists).
TEST_F(IndexReaderAuxTest, ivfListsAreServedFromFileView) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  constexpr int N = 80;
  for (int i = 0; i < N; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i), "embedding_v",
                    std::vector<float>{(float)(i % 10), (float)(i / 10), 1.0f, 0.5f}));
  }
  h.commit({"*"});

  auto reader = std::make_shared<IndexReader>(h.getIndexWriter()->dir);
  auto aux = firstSegmentAux(*reader, "vec.embedding_v");
  ASSERT_NE(aux, nullptr);
  auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
  ASSERT_NE(vaux, nullptr);

  auto* ivf = dynamic_cast<faiss::IndexIVF*>(vaux->getFaissIndex());
  ASSERT_NE(ivf, nullptr);
  auto* lists = dynamic_cast<MmapInvertedLists*>(ivf->invlists);
  ASSERT_NE(lists, nullptr);  // ArrayInvertedLists here = deserialized copy
  EXPECT_EQ((int64_t)lists->compute_ntotal(), (int64_t)N);

  std::vector<int> seen(N, 0);
  for (size_t l = 0; l < lists->nlist; l++) {
    size_t sz = lists->list_size(l);
    const faiss::idx_t* ids = lists->get_ids(l);
    for (size_t j = 0; j < sz; j++) {
      ASSERT_GE(ids[j], 0);
      ASSERT_LT(ids[j], N);
      seen[(size_t)ids[j]]++;
    }
  }
  for (int i = 0; i < N; i++) {
    EXPECT_EQ(seen[i], 1) << "valueRank " << i;
  }
}

// Pins the per-index RAM contract the memory story relies on: faiss::read_index
// RECOMPUTES the IVFPQ precomputed distance table at open for METRIC_L2 +
// by_residual (nlist * M * 256 floats - the largest resident piece for L2
// fields), and allocates NOTHING for inner-product metrics.  Cosine maps to
// METRIC_INNER_PRODUCT over unit vectors, so cosine fields must stay
// table-free.  If a FAISS upgrade changes either side, this fails and the
// residency accounting (and docs) need a fresh look.
TEST_F(IndexReaderAuxTest, cosineSkipsL2PrecomputedTable) {
  IvfPqGuard guard;

  // Keeps the reader + aux alive alongside the borrowed index pointer (the
  // VectorAuxReader owns the faiss::Index and the mmap lists behind it).
  struct OpenedIvfPq {
    std::shared_ptr<IndexReader> reader;
    std::shared_ptr<AuxReader> aux;
    faiss::IndexIVFPQ* ivfpq = nullptr;
  };
  auto openIvfPq = [](CollectionHelper& h) {
    OpenedIvfPq out;
    out.reader = std::make_shared<IndexReader>(h.getIndexWriter()->dir);
    out.aux = firstSegmentAux(*out.reader, "vec.embedding_v");
    EXPECT_NE(out.aux, nullptr);
    auto* vaux = dynamic_cast<VectorAuxReader*>(out.aux.get());
    EXPECT_NE(vaux, nullptr);
    if (vaux != nullptr) {
      out.ivfpq = dynamic_cast<faiss::IndexIVFPQ*>(vaux->getFaissIndex());
    }
    return out;
  };
  auto indexDocs = [](CollectionHelper& h) {
    for (int i = 0; i < 80; i++) {
      h.index(flatdoc("id", "doc" + std::to_string(i), "embedding_v",
                      std::vector<float>{(float)(i % 10), (float)(i / 10), 1.0f, 0.5f}));
    }
    h.commit({"*"});
  };

  {
    CollectionHelper h("main");
    enableCosineOnVecSuffix(h.collection(), true);
    indexDocs(h);
    auto opened = openIvfPq(h);
    ASSERT_NE(opened.ivfpq, nullptr);
    EXPECT_EQ(opened.ivfpq->metric_type, faiss::METRIC_INNER_PRODUCT);
    EXPECT_EQ(opened.ivfpq->precomputed_table.size(), 0u);
  }
  {
    CollectionHelper h("main");
    h.clear();
    enableL2OnVecSuffix(h.collection());
    indexDocs(h);
    auto opened = openIvfPq(h);
    ASSERT_NE(opened.ivfpq, nullptr);
    EXPECT_EQ(opened.ivfpq->metric_type, faiss::METRIC_L2);
    EXPECT_GT(opened.ivfpq->precomputed_table.size(), 0u);
  }
}

TEST_F(IndexReaderAuxTest, cosineRawColumnSetsRescorePolicy) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  enableCosineOnVecSuffix(h.collection(), false);

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{2.0f, (float)(i + 1), 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto reader = std::make_shared<IndexReader>(h.getIndexWriter()->dir);
  auto aux = firstSegmentAux(*reader, "vec.embedding_v");
  ASSERT_NE(aux, nullptr);
  auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
  ASSERT_NE(vaux, nullptr);
  EXPECT_EQ(vaux->getMetric(), (int32_t)luxir::api::VectorMetric::COSINE);
  EXPECT_TRUE(vaux->shouldNormalizeColumnOnCosineRescore());
}

// No aux entries -> reader has no aux readers (default schema, no metric).
TEST_F(IndexReaderAuxTest, noAuxEntriesIsEmpty) {
  CollectionHelper h("main");

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 2, 3}),
          UpdateMessage::COMMIT);

  auto reader = std::make_shared<IndexReader>(h.getIndexWriter()->dir);
  EXPECT_EQ(reader->auxReaders().size(), 0u);
  EXPECT_EQ(reader->getAuxReader("vec.embedding_v"), nullptr);
}

// Verifies that an IndexReader opened after a tiny commit still sees the
// carried overlay for the surviving segment.
TEST_F(IndexReaderAuxTest, opensCleanlyAfterTinyCommitCarryForward) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  // First commit + build.
  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;

  // Sanity: aux file is referenced and present after the first commit.
  {
    auto reader = std::make_shared<IndexReader>(dir);
    ASSERT_NE(firstSegmentAux(*reader, "vec.embedding_v"), nullptr);
  }

  // Simulate the race: the IndexInfo we're about to parse references file F1,
  // but a concurrent commit's cleanup has already deleted F1.  We approximate
  // this by physically deleting the aux file *and* publishing a newer commit
  // (which the retry will land on).  The retry loop should not crash and
  // should successfully resolve to the new commit's aux readers.
  std::string oldFile;
  {
    auto reader1 = std::make_shared<IndexReader>(dir);
    auto aux = firstSegmentAux(*reader1, "vec.embedding_v");
    ASSERT_NE(aux, nullptr);
    // Find the file referenced by this aux entry via the on-disk IndexInfo.
    auto loaded = readDurableIndexInfo(dir);
    const auto& auxInfo = onlyVectorOverlay(loaded.info);
    ASSERT_EQ(auxInfo.files.size(), 1u);
    oldFile = std::string(auxInfo.files[0]);
  }

  // Publish a tiny below-threshold segment.  It should not build a new ANN
  // overlay and should not disturb the old segment's overlay.
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0}));
  h.commit({"*"});

  EXPECT_NE(dir.openFile(oldFile), nullptr) << "carried aux file should survive";

  auto reader2 = std::make_shared<IndexReader>(dir);
  EXPECT_EQ(reader2->auxReaders().size(), 0u);  // index-level registry stays empty
  auto aux2 = firstSegmentAux(*reader2, "vec.embedding_v");
  ASSERT_NE(aux2, nullptr);
  auto* vaux2 = dynamic_cast<VectorAuxReader*>(aux2.get());
  ASSERT_NE(vaux2, nullptr);
  EXPECT_EQ(vaux2->getFaissIndex()->ntotal, 80);
}

// Real retry-trigger: physically delete the aux file referenced by the
// current IndexInfo *without* publishing a new commit.  IndexReader's first
// attempt sees the file missing (with missingFileOK=true), retries, re-parses
// the same IndexInfo (commitTime didn't advance), flips missingFileOK=false,
// and the second attempt throws filesystem_error.  This exercises the retry
// loop's escalation path end-to-end.
//
// Truly *successful* retries (where a concurrent commit republishes during
// the retry window) need a Directory wrapper that can simulate the race -
// not in scope for this test.
TEST_F(IndexReaderAuxTest, retryEscalatesWhenAuxFilePersistentlyMissing) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;

  // Find the aux file referenced by the current IndexInfo and delete it.
  std::string auxFile;
  {
    auto loaded = readDurableIndexInfo(dir);
    const auto& auxInfo = onlyVectorOverlay(loaded.info);
    ASSERT_EQ(auxInfo.files.size(), 1u);
    auxFile = std::string(auxInfo.files[0]);
  }
  ASSERT_TRUE(dir.deleteFile(auxFile));

  // IndexReader open should retry once (missing file -> re-parse), then on
  // the second attempt see the same commit time and escalate to throw.
  EXPECT_THROW(
    { auto r = std::make_shared<IndexReader>(dir); },
    std::filesystem::filesystem_error);

  // Clean up the deliberately-corrupted directory state. The shared "main"
  // collection persists across tests/benchmarks, so leaving the IndexInfo
  // referencing a deleted aux file would break any later code that opens
  // an IndexReader on this collection.
}

// Carry-forward reuse: when the new commit's segment overlay has the same
// name + gen as the previous reader's, IndexReader reuses the previous
// AuxReader instance instead of re-deserializing.  We verify by pointer identity.
TEST_F(IndexReaderAuxTest, reusesAuxReaderOnCarryForward) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  // Build aux index.
  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 1.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;
  auto reader1 = std::make_shared<IndexReader>(dir);
  auto aux1 = firstSegmentAux(*reader1, "vec.embedding_v");
  ASSERT_NE(aux1, nullptr);

  // Delete-only commit: the segment survives, so its overlay is carried by
  // segment liveness with identical name/gen.
  std::vector<std::string> ids{"doc0"};
  h.deleteByIds(ids, UpdateMessage::COMMIT);

  auto reader2 = std::make_shared<IndexReader>(dir, reader1.get());
  auto aux2 = firstSegmentAux(*reader2, "vec.embedding_v");
  ASSERT_NE(aux2, nullptr);

  // Same shared_ptr target - reused, not re-deserialized.
  EXPECT_EQ(aux1.get(), aux2.get())
    << "aux reader should be reused across reopens when the entry is carried forward";
}

// Adding an above-threshold segment builds a second overlay.  The surviving
// segment's reader is reused; the new segment gets a fresh reader.
TEST_F(IndexReaderAuxTest, newSegmentGetsFreshAuxReader) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  enableL2OnVecSuffix(h.collection());

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;
  auto reader1 = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader1->segments().size(), 1u);
  auto aux1 = reader1->segments()[0].getAuxReader("vec.embedding_v");
  ASSERT_NE(aux1, nullptr);

  // Add another above-threshold segment and build only its missing overlay.
  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{0.0f, (float)i, 1.0f, 0.0f}));
  }
  h.commit({"*"});

  auto reader2 = std::make_shared<IndexReader>(dir, reader1.get());
  ASSERT_EQ(reader2->segments().size(), 2u);
  auto aux2a = reader2->segments()[0].getAuxReader("vec.embedding_v");
  auto aux2b = reader2->segments()[1].getAuxReader("vec.embedding_v");
  ASSERT_NE(aux2a, nullptr);
  ASSERT_NE(aux2b, nullptr);
  EXPECT_EQ(aux1.get(), aux2a.get());
  EXPECT_NE(aux1.get(), aux2b.get());
  auto* vaux2 = dynamic_cast<VectorAuxReader*>(aux2b.get());
  ASSERT_NE(vaux2, nullptr);
  EXPECT_EQ(vaux2->getFaissIndex()->ntotal, 80);
}

TEST_F(IndexReaderAuxTest, testOverlayConcurrentOpenHammer) {
  CollectionHelper h("main");
  auto iw = h.getIndexWriter();
  iw->mergePolicy->setMergeFactor(100);

  for (int seg = 0; seg < 2; seg++) {
    h.index(flatdoc("id", "seed" + std::to_string(seg)));
    h.commit({std::string(TestOverlayAuxReader::NAME)});
  }

  auto& dir = iw->dir;
  auto oldReader = std::make_shared<IndexReader>(dir);
  ASSERT_GT(oldReader->segments().size(), 0u);
  for (const auto& seg : oldReader->segments()) {
    auto aux = seg.getAuxReader(TestOverlayAuxReader::NAME);
    ASSERT_NE(aux, nullptr);
    auto* testAux = dynamic_cast<TestOverlayAuxReader*>(aux.get());
    ASSERT_NE(testAux, nullptr);
    EXPECT_GT(testAux->size(), 0u);
  }

  iw->mergePolicy->setMergeFactor(2);
  iw->mergePolicy->refresh();

  std::atomic_bool stop{false};
  std::atomic_int failures{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; t++) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        try {
          auto reader = std::make_shared<IndexReader>(dir);
          for (const auto& seg : reader->segments()) {
            auto aux = seg.getAuxReader(TestOverlayAuxReader::NAME);
            if (!aux) continue;
            auto* testAux = dynamic_cast<TestOverlayAuxReader*>(aux.get());
            if (testAux == nullptr || testAux->size() == 0) {
              failures.fetch_add(1);
            }
          }
        } catch (const std::exception&) {
          failures.fetch_add(1);
        }
      }
    });
  }

  for (int i = 0; i < 20; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i)));
    h.commit({std::string(TestOverlayAuxReader::NAME)});
    if (i % 4 == 3) {
      iw->mergeSegments();
      h.commit({std::string(TestOverlayAuxReader::NAME)});
    }
  }

  stop.store(true);
  for (auto& thread : readers) thread.join();
  EXPECT_EQ(failures.load(), 0);

  for (const auto& seg : oldReader->segments()) {
    auto aux = seg.getAuxReader(TestOverlayAuxReader::NAME);
    ASSERT_NE(aux, nullptr);
    auto* testAux = dynamic_cast<TestOverlayAuxReader*>(aux.get());
    ASSERT_NE(testAux, nullptr);
    EXPECT_GT(testAux->size(), 0u);
  }
}

// Unknown aux kinds in IndexInfo are ignored, not treated as missing files.
// We construct an IndexInfo with an extra unknown aux entry by hand-writing
// a modified s.olux on top of an existing index.
TEST_F(IndexReaderAuxTest, unknownAuxKindIsSkipped) {
  CollectionHelper h("main");

  h.index(flatdoc("id", std::string("a")), UpdateMessage::COMMIT);

  auto& dir = h.getIndexWriter()->dir;

  // Read existing IndexInfo, append an unknown aux entry, write it back.  The concrete
  // IndexInfo is non-owning (aux_indexes / files are spans), so grow the arrays into the
  // loaded arena instead of emplace_back, then re-encode while the loaded view is alive.
  auto loaded = readDurableIndexInfo(dir);
  auto oldAux = loaded.info.aux_indexes;
  luxir::api::AuxIndexInfo* aux =
      luxir::api::build::allocArray(loaded.info.aux_indexes, oldAux.size() + 1, *loaded.arena);
  for (size_t i = 0; i < oldAux.size(); i++) aux[i] = oldAux[i];
  auto& extra = aux[oldAux.size()];
  extra.kind = "future_kind_abc";
  extra.name = "future.foo";
  extra.gen = 1;
  luxir::api::build::allocArray(extra.files, 1, *loaded.arena)[0] =
      "nonexistent_file_should_not_be_opened";

  {
    std::vector<std::byte> serialized;
    ASSERT_TRUE(luxir::api::encode(loaded.info, serialized));
    auto out = dir.createFile(Postings::INDEX_INFO_FILE);
    OutputStream os;
    os.setFile(&*out);
    os.write((const char*)serialized.data(), serialized.size());
    os.close();
    dir.finishFile(*out);
  }

  // Open should succeed: the unknown entry is skipped silently.
  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->auxReaders().size(), 0u);
  EXPECT_EQ(reader->getAuxReader("future.foo"), nullptr);
}
