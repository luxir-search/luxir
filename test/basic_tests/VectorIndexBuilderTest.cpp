#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "solux/index/VectorIndexBuilder.h"
#include "solux/schema/Schema.h"
#include "solux/server/SoluxNode.h"
#include "solux/store/Directory.h"
#include "solux/reader/Postings.h"
#include "protos/solux_types.pb.h"

#include <faiss/IndexFlat.h>
#include <faiss/MetricType.h>
#include <faiss/index_io.h>
#include <faiss/impl/io.h>

#include <google/protobuf/arena.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <cstring>
#include <memory>
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

struct VectorAuxMeta {
  int32_t dims;
  int32_t metric;
  int32_t cosineNormalizeColumnOnRescore;
};

VectorAuxMeta readVectorAuxMeta(const proto::AuxIndexInfo& aux) {
  VectorAuxMeta meta{};
  EXPECT_EQ(aux.opaque_meta().size(), sizeof(int32_t) * 3);
  if (aux.opaque_meta().size() >= sizeof(int32_t) * 3) {
    std::memcpy(&meta.dims, aux.opaque_meta().data(), sizeof(int32_t));
    std::memcpy(&meta.metric, aux.opaque_meta().data() + sizeof(int32_t), sizeof(int32_t));
    std::memcpy(&meta.cosineNormalizeColumnOnRescore,
                aux.opaque_meta().data() + 2 * sizeof(int32_t), sizeof(int32_t));
  }
  return meta;
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

} // namespace

// One segment, one field, build all aux indexes via "*".  Verify a FAISS index file
// is written with the expected ntotal / dims and that the IndexInfo references it.
TEST_F(VectorIndexBuilderTest, basicBuildSingleSegment) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  std::vector<std::vector<float>> vecs = {
    {1, 0, 0, 0},
    {0, 1, 0, 0},
    {0, 0, 1, 0},
    {0.5f, 0.5f, 0.5f, 0.5f},
  };
  for (size_t i = 0; i < vecs.size(); i++) {
    Doc d = flatdoc("id", "doc" + std::to_string(i), "embedding_v", vecs[i]);
    h.index(d);
  }

  // Trigger a commit that also rebuilds all aux indexes.
  h.commit({"*"});

  auto& shardDir = h.getIndexWriter()->dir;

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(shardDir, arena);
  ASSERT_EQ(1, info->aux_indexes_size());
  const auto& aux = info->aux_indexes(0);
  EXPECT_EQ(aux.kind(), "vector_faiss");
  EXPECT_EQ(aux.field(), "embedding_v");
  EXPECT_EQ(aux.name(), "vec.embedding_v");
  ASSERT_EQ(aux.files_size(), 1);

  // Read and verify the FAISS index.
  auto idx = readFaissIndex(shardDir, aux.files(0));
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->d, 4);
  EXPECT_EQ(idx->ntotal, (faiss::idx_t)vecs.size());
  EXPECT_EQ(idx->metric_type, faiss::METRIC_L2);

  // Sanity: kNN search for the first vector should find itself at distance 0.
  std::vector<faiss::idx_t> ids(1);
  std::vector<float> dists(1);
  idx->search(1, vecs[0].data(), 1, dists.data(), ids.data());
  EXPECT_EQ(ids[0], 0);
  EXPECT_FLOAT_EQ(dists[0], 0.0f);
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
}

// Multiple segments - ntotal must equal the live-doc count across segments,
// and the metric file's mapping should round-trip (segId, localDoc) correctly.
TEST_F(VectorIndexBuilderTest, buildAcrossMultipleSegments) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  // Three segments, each committed (no aux build) so they end up as separate segments.
  for (int seg = 0; seg < 3; seg++) {
    for (int i = 0; i < 3; i++) {
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
  ASSERT_EQ(1, info->aux_indexes_size());
  const auto& aux = info->aux_indexes(0);

  ASSERT_EQ(aux.files_size(), 1);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, aux.files(0));
  EXPECT_EQ(idx->d, 3);
  EXPECT_EQ(idx->ntotal, 9);

  // opaque_meta carries dims + metric + cosine rescore policy.
  auto meta = readVectorAuxMeta(aux);
  EXPECT_EQ(meta.dims, 3);
  EXPECT_EQ(meta.metric, (int32_t)proto::VectorParams::L2);
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
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
}

// A delete-only second commit doesn't change segment composition (coreGen),
// so the vector aux index built for the previous commit remains valid and is
// carried forward (its files are not deleted).
TEST_F(VectorIndexBuilderTest, carryForwardOnDeleteOnlyCommit) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  Doc d1 = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0});
  Doc d2 = flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0});
  h.index(d1);
  h.index(d2);
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  ASSERT_EQ(1, info1->aux_indexes_size());
  uint64_t builtCoreGen = info1->aux_indexes(0).built_core_gen();
  std::vector<std::string> origFiles(info1->aux_indexes(0).files().begin(),
                                     info1->aux_indexes(0).files().end());

  // Delete one doc and commit - segment composition unchanged.
  std::vector<std::string> ids{"a"};
  h.deleteByIds(ids, UpdateMessage::COMMIT);

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  ASSERT_EQ(1, info2->aux_indexes_size());
  EXPECT_EQ(info2->aux_indexes(0).name(), "vec.embedding_v");
  // built_core_gen survives unchanged.
  EXPECT_EQ(info2->aux_indexes(0).built_core_gen(), builtCoreGen);
  // Files survive on disk.
  for (const auto& f : origFiles) {
    EXPECT_NE(h.getIndexWriter()->dir.openFile(f), nullptr) << "carried file: " << f;
  }
}

// Adding a new segment without rebuilding invalidates the vector aux index
// (coreGen bumps).  The entry is dropped from IndexInfo and its files deleted.
TEST_F(VectorIndexBuilderTest, invalidatedOnSegmentChange) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  Doc d1 = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0});
  h.index(d1);
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  ASSERT_EQ(1, info1->aux_indexes_size());
  std::vector<std::string> origFiles(info1->aux_indexes(0).files().begin(),
                                     info1->aux_indexes(0).files().end());

  // Index more docs (creates a new segment) and commit without rebuild.
  Doc d2 = flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0});
  h.index(d2, UpdateMessage::COMMIT);

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  EXPECT_EQ(0, info2->aux_indexes_size());
  // Files from the now-invalid aux are deleted.
  for (const auto& f : origFiles) {
    EXPECT_EQ(h.getIndexWriter()->dir.openFile(f), nullptr) << "should be deleted: " << f;
  }
}

// A rebuild for a name should delete the previous gen's files for that name.
TEST_F(VectorIndexBuilderTest, rebuildDeletesPreviousFiles) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  Doc d1 = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0});
  h.index(d1);
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  std::vector<std::string> oldFiles(info1->aux_indexes(0).files().begin(),
                                    info1->aux_indexes(0).files().end());

  // Add a new doc and rebuild - produces a fresh gen of files.
  Doc d2 = flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0});
  h.index(d2);
  h.commit({"*"});

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  ASSERT_EQ(1, info2->aux_indexes_size());
  std::vector<std::string> newFiles(info2->aux_indexes(0).files().begin(),
                                    info2->aux_indexes(0).files().end());

  // Old files should be gone.
  for (const auto& f : oldFiles) {
    EXPECT_EQ(h.getIndexWriter()->dir.openFile(f), nullptr) << "old file should be deleted: " << f;
  }
  // New files should exist.
  for (const auto& f : newFiles) {
    EXPECT_NE(h.getIndexWriter()->dir.openFile(f), nullptr) << "new file should exist: " << f;
  }
  // The new gen's ntotal should reflect both docs.
  auto idx = readFaissIndex(h.getIndexWriter()->dir, info2->aux_indexes(0).files(0));
  EXPECT_EQ(idx->ntotal, 2);
}

// Cosine metric normalizes on column write by default.  The FAISS builder can
// add the already-normalized column bytes directly as inner-product vectors.
TEST_F(VectorIndexBuilderTest, cosineNormalizeOnWriteBuildsInnerProductIndex) {
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

  // Two doc vectors, NOT pre-normalized.  The write path stores normalized
  // vectors, so FAISS sees unit vectors.
  Doc d1 = flatdoc("id", std::string("a"), "v_v", std::vector<float>{2, 0, 0});
  Doc d2 = flatdoc("id", std::string("b"), "v_v", std::vector<float>{0, 5, 0});
  h.index(d1);
  h.index(d2);
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  ASSERT_EQ(1, info->aux_indexes_size());
  auto meta = readVectorAuxMeta(info->aux_indexes(0));
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, info->aux_indexes(0).files(0));
  EXPECT_EQ(idx->metric_type, faiss::METRIC_INNER_PRODUCT);

  // Unit-length query in doc-a's direction - IP against normalized stored
  // vectors should be 1.0 for doc-a, 0.0 for doc-b.
  std::vector<float> query{1, 0, 0};
  std::vector<faiss::idx_t> ids(2);
  std::vector<float> dists(2);
  idx->search(1, query.data(), 2, dists.data(), ids.data());
  EXPECT_EQ(ids[0], 0);
  EXPECT_NEAR(dists[0], 1.0f, 1e-5);
  EXPECT_NEAR(dists[1], 0.0f, 1e-5);
}

// normalized=true: writer and builder trust vectors are already unit-length and
// skip renormalization.
TEST_F(VectorIndexBuilderTest, normalizedFlagSkipsRenorm) {
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

  // Deliberately non-unit despite normalized=true: this verifies the flag is
  // trust-only and prevents both write-time and build-time renormalization.
  Doc d1 = flatdoc("id", std::string("a"), "v_v", std::vector<float>{2, 0, 0});
  Doc d2 = flatdoc("id", std::string("b"), "v_v", std::vector<float>{0, 3, 0});
  h.index(d1);
  h.index(d2);
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  ASSERT_EQ(1, info->aux_indexes_size());
  auto meta = readVectorAuxMeta(info->aux_indexes(0));
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 0);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, info->aux_indexes(0).files(0));
  EXPECT_EQ(idx->ntotal, 2);

  std::vector<float> query{1, 0, 0};
  std::vector<faiss::idx_t> ids(2);
  std::vector<float> dists(2);
  idx->search(1, query.data(), 2, dists.data(), ids.data());
  EXPECT_EQ(ids[0], 0);
  EXPECT_FLOAT_EQ(dists[0], 2.0f);
  EXPECT_FLOAT_EQ(dists[1], 0.0f);
}

// Requesting a rebuild when nothing has changed (coreGen unchanged, entry
// still valid) should NOT rewrite the file - the carried-forward entry's
// existing files survive untouched.
TEST_F(VectorIndexBuilderTest, rebuildSkippedWhenStillValid) {
  CollectionHelper h("main");
  h.clear();
  enableL2OnVecSuffix(h.collection());

  Doc d1 = flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0});
  Doc d2 = flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0});
  h.index(d1);
  h.index(d2);
  h.commit({"*"});

  google::protobuf::Arena arena1;
  auto* info1 = readIndexInfo(h.getIndexWriter()->dir, arena1);
  ASSERT_EQ(1, info1->aux_indexes_size());
  std::string origFile{info1->aux_indexes(0).files(0)};

  // Delete-only commit (coreGen unchanged) that *also* requests a rebuild via
  // "*".  The rebuild should be a no-op because the carried entry is still
  // valid; the original file should still be on disk and referenced.
  std::vector<std::string> ids{"a"};
  h.deleteByIds(ids);
  h.commit({"*"});

  google::protobuf::Arena arena2;
  auto* info2 = readIndexInfo(h.getIndexWriter()->dir, arena2);
  ASSERT_EQ(1, info2->aux_indexes_size());
  EXPECT_EQ(info2->aux_indexes(0).files(0), origFile)
      << "filename should be unchanged (no rebuild)";
  EXPECT_NE(h.getIndexWriter()->dir.openFile(origFile), nullptr);
}

// Cosine renormalization happens in fixed-size chunks so a single segment with
// many vectors doesn't blow up memory.  Set a tiny chunk size and verify the
// loop boundaries - every vector still ends up correctly normalized.
TEST_F(VectorIndexBuilderTest, cosineRenormChunkBoundaries) {
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

  // 7 vectors, each non-unit and pointing in different cardinal directions.
  // After normalization each should land at the corresponding unit vector.
  std::vector<std::vector<float>> vecs = {
    {3, 0, 0, 0}, {0, 5, 0, 0}, {0, 0, 7, 0}, {0, 0, 0, 9},
    {2, 2, 0, 0}, {0, 0, 4, 4}, {1, 1, 1, 1},
  };
  for (size_t i = 0; i < vecs.size(); i++) {
    Doc d = flatdoc("id", "doc" + std::to_string(i), "v_v", vecs[i]);
    h.index(d);
  }
  h.commit({"*"});

  google::protobuf::Arena arena;
  auto* info = readIndexInfo(h.getIndexWriter()->dir, arena);
  ASSERT_EQ(1, info->aux_indexes_size());
  auto meta = readVectorAuxMeta(info->aux_indexes(0));
  EXPECT_EQ(meta.cosineNormalizeColumnOnRescore, 1);
  auto idx = readFaissIndex(h.getIndexWriter()->dir, info->aux_indexes(0).files(0));
  ASSERT_EQ(idx->ntotal, (faiss::idx_t)vecs.size());

  // Each input, after L2 normalization, queried back against itself should
  // hit IP=1.0 - confirms every chunk got normalized correctly.
  for (size_t i = 0; i < vecs.size(); i++) {
    auto v = vecs[i];
    float norm = 0.0f;
    for (float x : v) norm += x * x;
    norm = std::sqrt(norm);
    for (float& x : v) x /= norm;

    std::vector<faiss::idx_t> ids(1);
    std::vector<float> dists(1);
    idx->search(1, v.data(), 1, dists.data(), ids.data());
    EXPECT_EQ(ids[0], (faiss::idx_t)i) << "vec " << i;
    EXPECT_NEAR(dists[0], 1.0f, 1e-5) << "vec " << i;
  }

  VectorIndexBuilder::renormChunkBytes = saved;
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
}
