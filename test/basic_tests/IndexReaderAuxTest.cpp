#include <gtest/gtest.h>

#include <faiss/Index.h>
#include <faiss/MetricType.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "protos/solux_types.pb.h"
#include "solux/index/IndexWriter.h"
#include "solux/index/VectorIndexBuilder.h"
#include "solux/reader/AuxReader.h"
#include "solux/reader/Postings.h"
#include "solux/reader/TestOverlayAuxReader.h"
#include "solux/reader/VectorAuxReader.h"
#include "solux/schema/Schema.h"
#include "solux/search/IndexReader.h"
#include "solux/server/SoluxNode.h"
#include "test/CollectionHelper.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

class IndexReaderAuxTest : public SoluxTest {
protected:
  void SetUp() override {
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
    proto::SchemaDef def;
    auto* f = def.add_fields();
    f->set_name("_v");
    f->set_field_class(proto::FieldDef::VECTOR);
    f->set_abstract(true);
    f->set_column_stored(true);
    f->mutable_vector()->set_metric(proto::VectorParams::L2);
    auto base = col.getSchema();
    col.setSchema(Schema::fromProto(def, base.get()));
  }

  static void enableCosineOnVecSuffix(Collection& col, bool normalizeOnWrite) {
    proto::SchemaDef def;
    auto* f = def.add_fields();
    f->set_name("_v");
    f->set_field_class(proto::FieldDef::VECTOR);
    f->set_abstract(true);
    f->set_column_stored(true);
    f->mutable_vector()->set_metric(proto::VectorParams::COSINE);
    f->mutable_vector()->set_normalize_on_write(normalizeOnWrite);
    auto base = col.getSchema();
    col.setSchema(Schema::fromProto(def, base.get()));
  }
};

namespace {

std::vector<const proto::AuxIndexInfo*> vectorOverlays(const proto::IndexInfo& info) {
  std::vector<const proto::AuxIndexInfo*> out;
  for (const auto& seg : info.segments()) {
    for (const auto& overlay : seg.overlays()) {
      if (overlay.kind() == VectorIndexBuilder::KIND) out.push_back(&overlay);
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

const proto::AuxIndexInfo& onlyVectorOverlay(const proto::IndexInfo& info) {
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
  h.clear();
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
  EXPECT_EQ(vaux->getMetric(), (int32_t)proto::VectorParams::L2);
  EXPECT_FALSE(vaux->shouldNormalizeColumnOnCosineRescore());

  auto* idx = vaux->getFaissIndex();
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->d, 4);
  EXPECT_EQ(idx->ntotal, (faiss::idx_t)vecs.size());
  EXPECT_EQ(idx->metric_type, faiss::METRIC_L2);
}

TEST_F(IndexReaderAuxTest, cosineRawColumnSetsRescorePolicy) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  h.clear();
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
  EXPECT_EQ(vaux->getMetric(), (int32_t)proto::VectorParams::COSINE);
  EXPECT_TRUE(vaux->shouldNormalizeColumnOnCosineRescore());
}

// No aux entries -> reader has no aux readers (default schema, no metric).
TEST_F(IndexReaderAuxTest, noAuxEntriesIsEmpty) {
  CollectionHelper h("main");
  h.clear();

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
  h.clear();
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
    auto infoFile = dir.openFile(Postings::INDEX_INFO_FILE);
    ASSERT_NE(infoFile, nullptr);
    proto::IndexInfo info;
    auto is = infoFile->getInputStream();
    ASSERT_TRUE(info.ParseFromArray(is.ptr(), (int)is.left()));
    const auto& auxInfo = onlyVectorOverlay(info);
    ASSERT_EQ(auxInfo.files_size(), 1);
    oldFile = auxInfo.files(0);
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
  h.clear();
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
    auto infoFile = dir.openFile(Postings::INDEX_INFO_FILE);
    ASSERT_NE(infoFile, nullptr);
    proto::IndexInfo info;
    auto is = infoFile->getInputStream();
    ASSERT_TRUE(info.ParseFromArray(is.ptr(), (int)is.left()));
    const auto& auxInfo = onlyVectorOverlay(info);
    ASSERT_EQ(auxInfo.files_size(), 1);
    auxFile = auxInfo.files(0);
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
  h.clear();
}

// Carry-forward reuse: when the new commit's segment overlay has the same
// name + gen as the previous reader's, IndexReader reuses the previous
// AuxReader instance instead of re-deserializing.  We verify by pointer identity.
TEST_F(IndexReaderAuxTest, reusesAuxReaderOnCarryForward) {
  IvfPqGuard guard;
  CollectionHelper h("main");
  h.clear();
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
  h.clear();
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
  h.clear();
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
  h.clear();

  h.index(flatdoc("id", std::string("a")), UpdateMessage::COMMIT);

  auto& dir = h.getIndexWriter()->dir;

  // Read existing IndexInfo, append an unknown aux entry, write it back.
  proto::IndexInfo info;
  {
    auto f = dir.openFile(Postings::INDEX_INFO_FILE);
    ASSERT_NE(f, nullptr);
    auto is = f->getInputStream();
    ASSERT_TRUE(info.ParseFromArray(is.ptr(), (int)is.left()));
  }
  auto* extra = info.add_aux_indexes();
  extra->set_kind("future_kind_abc");
  extra->set_name("future.foo");
  extra->set_gen(1);
  extra->add_files("nonexistent_file_should_not_be_opened");

  {
    auto out = dir.createFile(Postings::INDEX_INFO_FILE);
    std::string serialized = info.SerializeAsString();
    OutputStream os;
    os.setFile(&*out);
    os.write(serialized.data(), serialized.size());
    os.close();
    dir.finishFile(*out);
  }

  // Open should succeed: the unknown entry is skipped silently.
  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->auxReaders().size(), 0u);
  EXPECT_EQ(reader->getAuxReader("future.foo"), nullptr);
}
