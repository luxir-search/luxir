#include <gtest/gtest.h>

#include <faiss/Index.h>
#include <faiss/MetricType.h>

#include <memory>
#include <string>
#include <vector>

#include "protos/solux_types.pb.h"
#include "solux/index/IndexWriter.h"
#include "solux/index/VectorIndexBuilder.h"
#include "solux/reader/AuxReader.h"
#include "solux/reader/Postings.h"
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
    VectorIndexBuilder::buildFaissFlatAuxIndexes = true;
    auto col = soluxNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }

  void TearDown() override {
    VectorIndexBuilder::buildFaissFlatAuxIndexes = false;
  }

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

// Minimal happy path: build an aux index, open an IndexReader, and verify the
// AuxReader is present, dispatched to VectorAuxReader, and the FAISS index
// round-trips through deserialize_index correctly.
TEST_F(IndexReaderAuxTest, opensVectorAuxAfterBuild) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  std::vector<std::vector<float>> vecs = {
    {1, 0, 0, 0},
    {0, 1, 0, 0},
    {0, 0, 1, 0},
  };
  for (size_t i = 0; i < vecs.size(); i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i), "embedding_v", vecs[i]));
  }
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;
  auto reader = std::make_shared<IndexReader>(dir);

  ASSERT_EQ(reader->auxReaders().size(), 1u);
  auto aux = reader->getAuxReader("vec.embedding_v");
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

  // Sanity: querying the first vector returns itself at distance 0.
  std::vector<faiss::idx_t> ids(1);
  std::vector<float> dists(1);
  idx->search(1, vecs[0].data(), 1, dists.data(), ids.data());
  EXPECT_EQ(ids[0], 0);
  EXPECT_FLOAT_EQ(dists[0], 0.0f);
}

TEST_F(IndexReaderAuxTest, cosineRawColumnSetsRescorePolicy) {
  CollectionHelper h("main");
  h.clear();
  enableCosineOnVecSuffix(h.collection(), false);

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{2, 0, 0}));
  h.commit({"*"});

  auto reader = std::make_shared<IndexReader>(h.getIndexWriter()->dir);
  auto aux = reader->getAuxReader("vec.embedding_v");
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

// Verifies that an IndexReader opened after a rebuild commit (which deletes
// the previous gen's aux file) finds the new file cleanly.  Does NOT actually
// trigger the retry loop - by the time the reader parses IndexInfo, it
// already references the new file, so the first open attempt succeeds.  See
// retryEscalatesWhenAuxFilePersistentlyMissing for a test that actually
// exercises the retry path.
TEST_F(IndexReaderAuxTest, opensCleanlyAfterRebuild) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  // First commit + build.
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;

  // Sanity: aux file is referenced and present after the first commit.
  {
    auto reader = std::make_shared<IndexReader>(dir);
    ASSERT_EQ(reader->auxReaders().size(), 1u);
  }

  // Simulate the race: the IndexInfo we're about to parse references file F1,
  // but a concurrent commit's cleanup has already deleted F1.  We approximate
  // this by physically deleting the aux file *and* publishing a newer commit
  // (which the retry will land on).  The retry loop should not crash and
  // should successfully resolve to the new commit's aux readers.
  std::string oldFile;
  {
    auto reader1 = std::make_shared<IndexReader>(dir);
    auto aux = reader1->getAuxReader("vec.embedding_v");
    ASSERT_NE(aux, nullptr);
    // Find the file referenced by this aux entry via the on-disk IndexInfo.
    auto infoFile = dir.openFile(Postings::INDEX_INFO_FILE);
    ASSERT_NE(infoFile, nullptr);
    proto::IndexInfo info;
    auto is = infoFile->getInputStream();
    ASSERT_TRUE(info.ParseFromArray(is.ptr(), (int)is.left()));
    ASSERT_EQ(info.aux_indexes_size(), 1);
    ASSERT_EQ(info.aux_indexes(0).files_size(), 1);
    oldFile = info.aux_indexes(0).files(0);
  }

  // Now publish a second commit that rebuilds (new file).  Both writes are
  // synchronous via CollectionHelper, so by the time it returns the new
  // IndexInfo + aux file are durable and the old aux file has been cleaned up
  // by deleteOrphanedAuxFiles.
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0}));
  h.commit({"*"});

  // Old file should be gone; new IndexReader opens cleanly against the new
  // commit's referenced file.
  EXPECT_EQ(dir.openFile(oldFile), nullptr) << "old aux file should be deleted";

  auto reader2 = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader2->auxReaders().size(), 1u);
  auto aux2 = reader2->getAuxReader("vec.embedding_v");
  ASSERT_NE(aux2, nullptr);
  auto* vaux2 = dynamic_cast<VectorAuxReader*>(aux2.get());
  ASSERT_NE(vaux2, nullptr);
  // Two docs across two segments -> ntotal == 2.
  EXPECT_EQ(vaux2->getFaissIndex()->ntotal, 2);
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
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
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
    ASSERT_EQ(info.aux_indexes_size(), 1);
    ASSERT_EQ(info.aux_indexes(0).files_size(), 1);
    auxFile = info.aux_indexes(0).files(0);
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

// Carry-forward reuse: when the new commit's aux entry has the same name +
// gen + built_core_gen as the previous reader's, IndexReader reuses the
// previous AuxReader instance instead of re-deserializing.  We verify by
// checking pointer identity.
TEST_F(IndexReaderAuxTest, reusesAuxReaderOnCarryForward) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  // Build aux index.
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0}));
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;
  auto reader1 = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader1->auxReaders().size(), 1u);
  auto aux1 = reader1->getAuxReader("vec.embedding_v");
  ASSERT_NE(aux1, nullptr);

  // Delete-only commit: segment composition unchanged -> coreGen unchanged ->
  // aux entry carried forward with identical name/gen/built_core_gen.
  std::vector<std::string> ids{"a"};
  h.deleteByIds(ids, UpdateMessage::COMMIT);

  auto reader2 = std::make_shared<IndexReader>(dir, reader1.get());
  ASSERT_EQ(reader2->auxReaders().size(), 1u);
  auto aux2 = reader2->getAuxReader("vec.embedding_v");
  ASSERT_NE(aux2, nullptr);

  // Same shared_ptr target - reused, not re-deserialized.
  EXPECT_EQ(aux1.get(), aux2.get())
    << "aux reader should be reused across reopens when the entry is carried forward";
}

// New build (different gen / different built_core_gen) -> previous reader's
// aux is NOT reused; a fresh AuxReader is constructed.
TEST_F(IndexReaderAuxTest, rebuildsAuxReaderOnNewGen) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;
  auto reader1 = std::make_shared<IndexReader>(dir);
  auto aux1 = reader1->getAuxReader("vec.embedding_v");
  ASSERT_NE(aux1, nullptr);

  // Add a doc + rebuild - produces a new gen of files; coreGen also bumps
  // because segment composition changed.
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0}));
  h.commit({"*"});

  auto reader2 = std::make_shared<IndexReader>(dir, reader1.get());
  auto aux2 = reader2->getAuxReader("vec.embedding_v");
  ASSERT_NE(aux2, nullptr);
  EXPECT_NE(aux1.get(), aux2.get())
    << "aux reader should be re-deserialized when gen/built_core_gen change";
  auto* vaux2 = dynamic_cast<VectorAuxReader*>(aux2.get());
  ASSERT_NE(vaux2, nullptr);
  EXPECT_EQ(vaux2->getFaissIndex()->ntotal, 2);
}

// Unknown aux kinds in IndexInfo are ignored, not treated as missing files.
// We construct an IndexInfo with an extra unknown aux entry by hand-writing
// a modified s.olux on top of an existing index.
TEST_F(IndexReaderAuxTest, unknownAuxKindIsSkipped) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.commit({"*"});

  auto& dir = h.getIndexWriter()->dir;

  // Read existing IndexInfo, append an unknown aux entry, write it back.
  proto::IndexInfo info;
  {
    auto f = dir.openFile(Postings::INDEX_INFO_FILE);
    ASSERT_NE(f, nullptr);
    auto is = f->getInputStream();
    ASSERT_TRUE(info.ParseFromArray(is.ptr(), (int)is.left()));
  }
  ASSERT_EQ(info.aux_indexes_size(), 1);
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

  // Open should succeed: known entry present, unknown one skipped silently.
  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->auxReaders().size(), 1u);
  EXPECT_NE(reader->getAuxReader("vec.embedding_v"), nullptr);
  EXPECT_EQ(reader->getAuxReader("future.foo"), nullptr);
}
