#include "gtest/gtest.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <unistd.h>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "solux/index/handler/VectorHandler.h"
#include "solux/reader/VectorReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/schema/FieldType.h"
#include "solux/schema/Schema.h"
#include "solux/store/FSDirectory.h"
#include "solux/util/log.h"
#include "solux/api/build.h"

#include <memory_resource>

using namespace solux;
using namespace solux::test;

class VectorColTest : public SoluxTest {
protected:
  void SetUp() override {
    SoluxTest::SetUp();
    auto col = soluxNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }
};

// Build a Val containing a single Vector{f32}, backed by `mr` (must outlive the Val).
static solux::api::Val makeVec(std::pmr::memory_resource& mr, std::initializer_list<float> floats) {
  solux::api::Val v;
  auto& f32 = v.kind.emplace<solux::api::Vector>().f32.emplace();
  float* a = solux::api::build::allocArray(f32.v, floats.size(), mr);
  std::size_t i = 0;
  for (float f : floats) a[i++] = f;
  return v;
}

static void indexVal(Inverter& inverter, Inverter::IndexHandler& handler, const solux::api::Val& val) {
  handler.index(inverter, val);
}

static void enableCosineOnVecSuffix(Collection& col, bool normalizeOnWrite = true) {
  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = solux::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = solux::api::VectorMetric::COSINE;
  f.normalize_on_write = normalizeOnWrite;
  b.set(col);
}

// Index a multi-valued vector value (arr_vec) for the given doc.
static void indexMultiVec(Inverter& inverter, Inverter::IndexHandler& handler,
                          int32_t docid, std::vector<std::vector<float>> vecs) {
  std::pmr::monotonic_buffer_resource mr;
  inverter.setDoc(docid);
  solux::api::Val v;
  auto& arr = v.kind.emplace<solux::api::ArrVector>();
  auto* a = solux::api::build::allocArray(arr.v, vecs.size(), mr);
  for (std::size_t i = 0; i < vecs.size(); i++) {
    auto& f32 = a[i].f32.emplace();
    float* fa = solux::api::build::allocArray(f32.v, vecs[i].size(), mr);
    for (std::size_t j = 0; j < vecs[i].size(); j++) fa[j] = vecs[i][j];
  }
  indexVal(inverter, handler, v);
}

// Low-level round-trip: index single-valued vectors, flush, read back via VectorReader.
TEST_F(VectorColTest, singleValuedRoundTrip) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("vec_v");

  std::vector<std::vector<float>> expected = {
    {1.0f, 2.0f, 3.0f, 4.0f},
    {0.5f, -1.5f, 2.5f, -3.5f},
    {0.0f, 0.0f, 0.0f, 0.0f},
  };

  std::pmr::monotonic_buffer_resource mr;
  for (size_t doc = 0; doc < expected.size(); doc++) {
    inverter.setDoc((int32_t)doc);
    auto val = makeVec(mr, {expected[doc][0], expected[doc][1], expected[doc][2], expected[doc][3]});
    indexVal(inverter, handler, val);
  }
  testIndex.flush();

  testIndex.initReader();
  auto segments = testIndex.reader->segments();
  ASSERT_EQ(1u, segments.size());
  auto& seg = segments[0];
  FieldReader fieldReader(testIndex.pool, seg.postingsReader());
  ASSERT_TRUE(fieldReader.seek("vec_v"));
  SegFieldInfo fi;
  fieldReader.readFieldInfo(fi);

  VectorReader vr(seg.postingsReader(), fi);
  ASSERT_EQ(4, vr.dims());
  ASSERT_EQ((int32_t)expected.size(), vr.docsWithValue());
  ASSERT_FALSE(vr.isMultiValued());
  // Single-valued fields never get a valueRank->docId map (valueRank == docRank).
  ASSERT_FALSE(vr.hasValDocMap());

  for (int32_t docRank = 0; docRank < (int32_t)expected.size(); docRank++) {
    auto span = vr.singleVectorAt(docRank);
    ASSERT_EQ(4u, span.size());
    for (size_t i = 0; i < 4; i++) {
      EXPECT_FLOAT_EQ(expected[docRank][i], span[i]);
    }
  }
}

// Vector payloads go straight to a segment output; only their metadata streams
// consume inverter.pool, and the raw float bytes do not inflate extraRamBytes.
TEST_F(VectorColTest, payloadStreamsWithoutRamFile) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("vec_v");

  std::pmr::monotonic_buffer_resource mr;
  solux::api::Val val;
  auto& f32 = val.kind.emplace<solux::api::Vector>().f32.emplace();
  constexpr size_t DIMS = 256 * 1024;
  float* values = solux::api::build::allocArray(f32.v, DIMS, mr);
  for (size_t i = 0; i < DIMS; i++) values[i] = (float)i;

  size_t extraBefore = inverter.extraRamBytes;
  inverter.setDoc(0);
  indexVal(inverter, handler, val);
  EXPECT_EQ(extraBefore, inverter.extraRamBytes);

  testIndex.flush();
  testIndex.initReader();
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fieldReader(testIndex.pool, seg.postingsReader());
  ASSERT_TRUE(fieldReader.seek("vec_v"));
  SegFieldInfo fi;
  fieldReader.readFieldInfo(fi);
  VectorReader reader(seg.postingsReader(), fi);
  ASSERT_EQ((int32_t)DIMS, reader.dims());
  EXPECT_FLOAT_EQ(0.0f, reader.singleVectorAt(0).front());
  EXPECT_FLOAT_EQ((float)(DIMS - 1), reader.singleVectorAt(0).back());
}

// A filesystem failure can happen after a prefix of one vector has been
// written. It is segment-fatal: the inverter is discarded rather than reused
// for the next request.
TEST_F(VectorColTest, ioFailureAbortsStreamedSegment) {
  std::string pathTemplate =
      (std::filesystem::temp_directory_path() / "solux_vector_io_XXXXXX").string();
  ASSERT_NE(nullptr, ::mkdtemp(pathTemplate.data()));
  std::filesystem::path path(pathTemplate);

  {
    FSDirectory dir(path);
    IndexWriter iw(dir);

    auto submit = [&](bool commit) {
      std::pmr::monotonic_buffer_resource mr;
      solux::api::UpdateRequest request;
      auto* docs = solux::api::build::allocArray(request.docs, 1, mr);
      CollectionHelper::convertDocToProto(
          flatdoc("vec_v", std::vector<float>(512, 1.0f)), docs[0], mr);
      if (commit) request.commit.emplace();

      class BlockingMessage : public ProtoUpdateMessage {
      public:
        Blocker blocker;
        explicit BlockingMessage(const RequestProto* request) : ProtoUpdateMessage(request) {}
        void done(IndexWriter& iw) override {
          unused(iw);
          blocker.notify();
        }
      } message(&request);

      bool accepted = iw.submitUpdate(&message);
      EXPECT_TRUE(accepted);
      if (!accepted) return false;
      message.blocker.wait();
      return message.result.ok();
    };

    std::string firstFile = Postings::getIndexFileName(Postings::getSortableString(1), 0);
    std::filesystem::path failingTmp = path / (firstFile + ".tmp");
    std::filesystem::create_symlink("/dev/full", failingTmp);

    EXPECT_FALSE(submit(false));
    iw.updateGraph.wait_for_all();
    EXPECT_TRUE(iw.segInfos.empty());
    EXPECT_TRUE(iw.idleInverters.empty());

    // FSDirectory intentionally ignores non-regular entries during its prefix
    // sweep. Remove the test-only device symlink before the healthy request.
    std::filesystem::remove(failingTmp);
    EXPECT_TRUE(submit(true));
    iw.updateGraph.wait_for_all();
    EXPECT_EQ(1, iw.getIndexReader()->maxDoc());
  }

  std::filesystem::remove_all(path);
}

// Stored fields and vectors each hold a stream while documents are indexed.
// Both must release it before the full-text field checks out its three streams,
// allowing the segment to stay at that three-stream high-water mark.
TEST_F(VectorColTest, indexingStreamsReleasedBeforeFieldFlush) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  uint64_t segId = inverter.getSegId();

  inverter.startDoc();
  inverter.getIndexHandler("body_w").index(inverter, std::string_view("one two three"));
  std::pmr::monotonic_buffer_resource mr;
  auto val = makeVec(mr, {1.0f, 2.0f, 3.0f});
  inverter.getIndexHandler("vec_v").index(inverter, val);
  inverter.finishDoc();
  testIndex.flush();

  std::vector<std::string> files;
  testIndex.dir.listFiles(files);
  std::string dataPrefix = Postings::getIndexFileNamePrefix(segId) + "_";
  size_t dataFiles = std::count_if(files.begin(), files.end(), [&](const std::string& file) {
    return file.starts_with(dataPrefix);
  });
  EXPECT_EQ(3u, dataFiles);
}

// Low-level: multi-valued vector round-trip using the default "_vs" suffix.
TEST_F(VectorColTest, multiValuedRoundTrip) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("emb_vs");

  // doc 0: 2 vectors, doc 1: 1 vector, doc 2: 3 vectors
  std::vector<std::vector<std::vector<float>>> expected = {
    {{1, 2, 3}, {4, 5, 6}},
    {{7, 8, 9}},
    {{10, 11, 12}, {13, 14, 15}, {16, 17, 18}},
  };

  for (size_t doc = 0; doc < expected.size(); doc++) {
    indexMultiVec(inverter, handler, (int32_t)doc, expected[doc]);
  }
  testIndex.flush();

  testIndex.initReader();
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fieldReader(testIndex.pool, seg.postingsReader());
  ASSERT_TRUE(fieldReader.seek("emb_vs"));
  SegFieldInfo fi;
  fieldReader.readFieldInfo(fi);

  VectorReader vr(seg.postingsReader(), fi);
  ASSERT_EQ(3, vr.dims());
  ASSERT_TRUE(vr.isMultiValued());
  ASSERT_EQ((int32_t)expected.size(), vr.docsWithValue());

  int64_t expectedTotal = 0;
  for (auto& doc : expected) expectedTotal += (int64_t)doc.size();
  ASSERT_EQ(expectedTotal, vr.numVectors());

  for (int32_t docRank = 0; docRank < (int32_t)expected.size(); docRank++) {
    auto [start, end] = vr.valueRange(docRank);
    ASSERT_EQ((int64_t)expected[docRank].size(), end - start);
    for (int64_t v = start; v < end; v++) {
      auto span = vr.vectorAtRank(v);
      ASSERT_EQ(3u, span.size());
      auto& exp = expected[docRank][v - start];
      for (size_t i = 0; i < 3; i++) {
        EXPECT_FLOAT_EQ(exp[i], span[i]);
      }
    }
  }
}

// valueRank -> docId reverse map for multi-valued vectors.  Uses sparse doc ids
// (gaps) so the test fails if the map stored a dense rank-among-docs-with-field
// instead of the real segment-local docId.
TEST_F(VectorColTest, multiValuedValueRankToDoc) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("emb_vs");

  // doc 0: 2 vectors, doc 2: 1 vector, doc 5: 3 vectors.  Docs 1,3,4 have none.
  struct DocVecs { int32_t docId; std::vector<std::vector<float>> vecs; };
  std::vector<DocVecs> docs = {
    {0, {{1, 2}, {3, 4}}},
    {2, {{5, 6}}},
    {5, {{7, 8}, {9, 10}, {11, 12}}},
  };

  for (auto& dv : docs) {
    indexMultiVec(inverter, handler, dv.docId, dv.vecs);
  }
  testIndex.flush();

  testIndex.initReader();
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fieldReader(testIndex.pool, seg.postingsReader());
  ASSERT_TRUE(fieldReader.seek("emb_vs"));
  SegFieldInfo fi;
  fieldReader.readFieldInfo(fi);

  VectorReader vr(seg.postingsReader(), fi);
  ASSERT_TRUE(vr.isMultiValued());
  ASSERT_TRUE(vr.hasValDocMap());
  ASSERT_EQ(6, vr.numVectors());

  // Expected valueRank -> docId, in value-rank (doc) order.
  std::vector<int32_t> expectedDoc;
  for (auto& dv : docs)
    for (size_t i = 0; i < dv.vecs.size(); i++) expectedDoc.push_back(dv.docId);
  ASSERT_EQ(6u, expectedDoc.size());

  for (int64_t r = 0; r < vr.numVectors(); r++) {
    EXPECT_EQ(expectedDoc[(size_t)r], vr.docForVectorRank(r)) << "valueRank " << r;
  }
}

// The valueRank -> docId map must survive a segment merge: mergeStrCol regenerates
// it against the merged (remapped) doc ids.  Two multi-valued segments are merged
// and every value rank is checked against its owning merged docId.
TEST_F(VectorColTest, multiValuedValDocSurvivesMerge) {
  TestIndex testIndex;

  // Segment 1: doc 0 has 2 vectors, doc 1 has 1 vector.
  {
    auto& inverter = testIndex.getInverter();
    auto& handler = inverter.getIndexHandler("emb_vs");
    indexMultiVec(inverter, handler, 0, {{1, 2}, {3, 4}});
    indexMultiVec(inverter, handler, 1, {{5, 6}});
  }
  testIndex.flush();

  // Segment 2: doc 0 has 2 vectors.
  {
    auto& inverter = testIndex.getInverter();
    auto& handler = inverter.getIndexHandler("emb_vs");
    indexMultiVec(inverter, handler, 0, {{7, 8}, {9, 10}});
  }
  testIndex.flush();

  testIndex.iw->mergeSegments();
  testIndex.initReader();
  ASSERT_EQ(1u, testIndex.reader->segments().size());
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fieldReader(testIndex.pool, seg.postingsReader());
  ASSERT_TRUE(fieldReader.seek("emb_vs"));
  SegFieldInfo fi;
  fieldReader.readFieldInfo(fi);

  VectorReader vr(seg.postingsReader(), fi);
  ASSERT_TRUE(vr.isMultiValued());
  ASSERT_TRUE(vr.hasValDocMap());
  ASSERT_EQ(5, vr.numVectors());

  // merged docRanks: seg1 doc0->0, seg1 doc1->1, seg2 doc0->2.
  // value ranks: 0,1 -> doc 0; 2 -> doc 1; 3,4 -> doc 2.  (Identity would wrongly
  // map value rank 2 -> doc 2.)
  std::vector<int32_t> expectedDoc = {0, 0, 1, 2, 2};
  for (int64_t r = 0; r < vr.numVectors(); r++) {
    EXPECT_EQ(expectedDoc[(size_t)r], vr.docForVectorRank(r)) << "valueRank " << r;
  }

  // Sanity: the vectors themselves survived the merge intact and in order.
  std::vector<std::vector<float>> expectedVecs = {{1, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10}};
  for (int64_t r = 0; r < vr.numVectors(); r++) {
    auto span = vr.vectorAtRank(r);
    ASSERT_EQ(2u, span.size());
    EXPECT_FLOAT_EQ(expectedVecs[(size_t)r][0], span[0]);
    EXPECT_FLOAT_EQ(expectedVecs[(size_t)r][1], span[1]);
  }
}

// Dims inference: schema doesn't declare dims, first value wins; mismatched throws.
TEST_F(VectorColTest, dimsInferredFromFirstValue) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("vec_v");

  std::pmr::monotonic_buffer_resource mr;
  inverter.setDoc(0);
  auto v0 = makeVec(mr, {1, 2, 3, 4, 5, 6});
  indexVal(inverter, handler, v0);

  inverter.setDoc(1);
  auto v1 = makeVec(mr, {7, 8, 9, 10, 11});  // wrong dims
  EXPECT_THROW(indexVal(inverter, handler, v1), std::runtime_error);

  auto* vh = dynamic_cast<handler::VectorHandler*>(&handler);
  ASSERT_NE(nullptr, vh);
  EXPECT_EQ(6, vh->dims());
}

// Strict dims: when the FieldType declares dims=N, mismatched values are rejected
// even on the first index call (no inference window).  We construct the handler
// directly with a strict VectorFieldType to avoid plumbing a custom schema.
TEST_F(VectorColTest, strictDimsRejectsMismatch) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto strictType = std::make_shared<VectorFieldType>("strict_v", /*dims=*/4);
  handler::VectorHandler vh(inverter, "strict_v", strictType);

  std::pmr::monotonic_buffer_resource mr;
  inverter.setDoc(0);
  auto wrong = makeVec(mr, {1, 2, 3});  // 3 dims vs declared 4
  EXPECT_THROW(indexVal(inverter, vh, wrong), std::runtime_error);

  inverter.setDoc(1);
  auto right = makeVec(mr, {1, 2, 3, 4});
  EXPECT_NO_THROW(indexVal(inverter, vh, right));
}

// gRPC round trip (single-valued): index via CollectionHelper, retrieve via LocalReq.
TEST_F(VectorColTest, grpcSingleFieldsRoundTrip) {
  CollectionHelper h("main");

  Doc doc1 = flatdoc("id", std::string("a"), "vec_v", std::vector<float>{1.0f, 2.0f, 3.0f});
  Doc doc2 = flatdoc("id", std::string("b"), "vec_v", std::vector<float>{4.5f, -5.5f, 6.5f});
  h.index(doc1);
  h.index(doc2, UpdateMessage::COMMIT);

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "vec_v"}).limit(10);
  req->execute();
  ASSERT_OK(req);

  auto docs = req->getDocs();
  ASSERT_EQ(2u, docs.size());
  // Results may come back in any order; check both ids are present with matching vectors.
  bool sawA = false, sawB = false;
  for (auto& d : docs) {
    auto* idVal = find(d, "id");
    auto* vecVal = find(d, "vec_v");
    if (!idVal) continue;
    const auto& id = std::get<std::string>(*idVal);
    const auto& vec = std::get<std::vector<float>>(*vecVal);
    if (id == "a") {
      sawA = true;
      ASSERT_EQ(3u, vec.size());
      EXPECT_FLOAT_EQ(1.0f, vec[0]);
      EXPECT_FLOAT_EQ(2.0f, vec[1]);
      EXPECT_FLOAT_EQ(3.0f, vec[2]);
    } else if (id == "b") {
      sawB = true;
      ASSERT_EQ(3u, vec.size());
      EXPECT_FLOAT_EQ(4.5f, vec[0]);
      EXPECT_FLOAT_EQ(-5.5f, vec[1]);
      EXPECT_FLOAT_EQ(6.5f, vec[2]);
    }
  }
  EXPECT_TRUE(sawA);
  EXPECT_TRUE(sawB);

}

// gRPC round trip (multi-valued): exercises the loadVectorColForSegmentMulti path.
TEST_F(VectorColTest, grpcMultiFieldsRoundTrip) {
  CollectionHelper h("main");

  std::vector<std::vector<float>> aVecs = {{1, 2}, {3, 4}, {5, 6}};
  std::vector<std::vector<float>> bVecs = {{7, 8}};
  Doc doc1 = flatdoc("id", std::string("a"), "emb_vs", aVecs);
  Doc doc2 = flatdoc("id", std::string("b"), "emb_vs", bVecs);
  h.index(doc1);
  h.index(doc2, UpdateMessage::COMMIT);

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "emb_vs"}).limit(10);
  req->execute();
  ASSERT_OK(req);

  auto docs = req->getDocs();
  ASSERT_EQ(2u, docs.size());
  bool sawA = false, sawB = false;
  for (auto& d : docs) {
    auto* idVal = find(d, "id");
    auto* vecsVal = find(d, "emb_vs");
    if (!idVal) continue;
    const auto& id = std::get<std::string>(*idVal);
    const auto& vecs = std::get<std::vector<std::vector<float>>>(*vecsVal);
    if (id == "a") {
      sawA = true;
      EXPECT_EQ(aVecs, vecs);
    } else if (id == "b") {
      sawB = true;
      EXPECT_EQ(bVecs, vecs);
    }
  }
  EXPECT_TRUE(sawA);
  EXPECT_TRUE(sawB);

}

TEST_F(VectorColTest, cosineDefaultsToNormalizedColumnStorage) {
  CollectionHelper h("main");

  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = solux::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = solux::api::VectorMetric::COSINE;
  b.set(h.collection());

  Doc doc = flatdoc("id", std::string("a"), "vec_v", std::vector<float>{3.0f, 4.0f});
  h.index(doc, UpdateMessage::COMMIT);

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "vec_v"}).limit(10);
  req->execute();
  ASSERT_OK(req);

  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  auto* vecVal = find(docs[0], "vec_v");
  ASSERT_NE(nullptr, vecVal);
  const auto& vec = std::get<std::vector<float>>(*vecVal);
  ASSERT_EQ(2u, vec.size());
  EXPECT_FLOAT_EQ(0.6f, vec[0]);
  EXPECT_FLOAT_EQ(0.8f, vec[1]);

}

TEST_F(VectorColTest, cosineNormalizeOnWriteFalseKeepsRawColumnStorage) {
  CollectionHelper h("main");
  enableCosineOnVecSuffix(h.collection(), false);

  Doc doc = flatdoc("id", std::string("a"), "vec_v", std::vector<float>{3.0f, 4.0f});
  h.index(doc, UpdateMessage::COMMIT);

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "vec_v"}).limit(10);
  req->execute();
  ASSERT_OK(req);

  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  auto* vecVal = find(docs[0], "vec_v");
  ASSERT_NE(nullptr, vecVal);
  const auto& vec = std::get<std::vector<float>>(*vecVal);
  ASSERT_EQ(2u, vec.size());
  EXPECT_FLOAT_EQ(3.0f, vec[0]);
  EXPECT_FLOAT_EQ(4.0f, vec[1]);

}

TEST_F(VectorColTest, cosineNormalizedFlagKeepsRawColumnStorage) {
  CollectionHelper h("main");

  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = solux::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = solux::api::VectorMetric::COSINE;
  f.normalized = true;

  auto schema = b.set(h.collection());
  auto* ft = dynamic_cast<VectorFieldType*>(schema->getFieldTypePtr("vec_v"));
  ASSERT_NE(nullptr, ft);
  EXPECT_TRUE(ft->normalized());
  EXPECT_FALSE(ft->normalizeOnWrite());

  Doc doc = flatdoc("id", std::string("a"), "vec_v", std::vector<float>{3.0f, 4.0f});
  h.index(doc, UpdateMessage::COMMIT);

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "vec_v"}).limit(10);
  req->execute();
  ASSERT_OK(req);

  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  auto* vecVal = find(docs[0], "vec_v");
  ASSERT_NE(nullptr, vecVal);
  const auto& vec = std::get<std::vector<float>>(*vecVal);
  ASSERT_EQ(2u, vec.size());
  EXPECT_FLOAT_EQ(3.0f, vec[0]);
  EXPECT_FLOAT_EQ(4.0f, vec[1]);

}

// A zero / near-zero cosine vector has no direction; rather than fail the whole
// update we skip the value (the doc is indexed with no vector for that field).
TEST_F(VectorColTest, cosineSkipsZeroVector) {
  CollectionHelper h("main");

  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = solux::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = solux::api::VectorMetric::COSINE;
  b.set(h.collection());

  // Doc "a" has a zero vector (skipped); doc "b" has a usable one (kept).
  {
    ExpectLog quiet("skipping zero / near-zero vector");
    h.index(flatdoc("id", std::string("a"), "vec_v", std::vector<float>{0.0f, 0.0f}));
    h.index(flatdoc("id", std::string("b"), "vec_v", std::vector<float>{3.0f, 4.0f}),
            UpdateMessage::COMMIT);
  }

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "vec_v"}).limit(10);
  req->execute();
  ASSERT_OK(req);

  auto docs = req->getDocs();
  ASSERT_EQ(2u, docs.size());
  for (auto& doc : docs) {
    auto* id = find(doc, "id");
    ASSERT_NE(nullptr, id);
    auto* vecVal = find(doc, "vec_v");
    if (std::get<std::string>(*id) == "a") {
      EXPECT_EQ(nullptr, vecVal);  // zero vector was skipped
    } else {
      ASSERT_NE(nullptr, vecVal);  // usable vector was stored (normalized)
      const auto& vec = std::get<std::vector<float>>(*vecVal);
      ASSERT_EQ(2u, vec.size());
      EXPECT_FLOAT_EQ(0.6f, vec[0]);
      EXPECT_FLOAT_EQ(0.8f, vec[1]);
    }
  }

}

TEST_F(VectorColTest, cosineNormalizedFlagTrustsZeroVector) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto cosineType = std::make_shared<VectorFieldType>(
      "cos_v", /*dims=*/2, FieldType::COLUMN_STORED | FieldType::FIXED_SIZE,
      VectorFieldType::METRIC_COSINE, /*normalized=*/true);
  handler::VectorHandler vh(inverter, "cos_v", cosineType);

  std::pmr::monotonic_buffer_resource mr;
  inverter.setDoc(0);
  auto zero = makeVec(mr, {0.0f, 0.0f});
  EXPECT_NO_THROW(indexVal(inverter, vh, zero));
}

// Schema round-trip: toProto/fromProto preserves VECTOR field with dims.
TEST_F(VectorColTest, schemaProtoRoundTrip) {
  SchemaBuilder b;
  auto& f = b.field("embedding");
  f.type = solux::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.dims = 384;
  f.metric = solux::api::VectorMetric::COSINE;

  auto schema = b.build();
  auto it = schema->getFieldType("embedding");
  ASSERT_NE(it, schema->end());
  auto* vft = dynamic_cast<VectorFieldType*>(it->second.get());
  ASSERT_NE(nullptr, vft);
  EXPECT_EQ(384, vft->dims());
  EXPECT_EQ(VectorFieldType::METRIC_COSINE, vft->metric());
  EXPECT_TRUE(vft->normalizeOnWrite());
  EXPECT_TRUE(vft->hasColumn());
  EXPECT_TRUE(vft->isSet(FieldType::FIXED_SIZE));

  solux::api::SchemaDef outDef;
  std::pmr::monotonic_buffer_resource outMr;
  schema->toProto(&outDef, outMr);
  const auto* fd = outDef.fields.find("embedding");
  ASSERT_NE(nullptr, fd);
  ASSERT_TRUE(fd->type.has_value());
  EXPECT_EQ(solux::api::FieldDef_::FieldClass::VECTOR, *fd->type);
  ASSERT_TRUE(fd->dims.has_value());
  EXPECT_EQ(384, *fd->dims);
  ASSERT_TRUE(fd->metric.has_value());
  EXPECT_EQ(solux::api::VectorMetric::COSINE, *fd->metric);
  // toProto emits the authored sparse def: normalize_on_write was never set
  // (the on-write default is derived from COSINE), so it round-trips as absent.
  EXPECT_FALSE(fd->normalize_on_write.has_value());
}
