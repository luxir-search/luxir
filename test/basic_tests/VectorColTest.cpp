// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <limits>
#include <unistd.h>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "luxir/index/handler/VectorHandler.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/schema/FieldType.h"
#include "luxir/schema/Schema.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/util/log.h"
#include "luxir/api/build.h"

#include <memory_resource>

using namespace luxir;
using namespace luxir::test;

class VectorColTest : public LuxirTest {
protected:
  void SetUp() override {
    LuxirTest::SetUp();
    auto col = luxirNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }
};

// Build a Val containing a single Vector{f32}, backed by `mr` (must outlive the Val).
static luxir::api::Val makeVec(std::pmr::memory_resource& mr, std::initializer_list<float> floats) {
  luxir::api::Val v;
  auto& f32 = v.kind.emplace<luxir::api::Vector>().f32.emplace();
  float* a = luxir::api::build::allocArray(f32.v, floats.size(), mr);
  std::size_t i = 0;
  for (float f : floats) a[i++] = f;
  return v;
}

static luxir::api::Val makeDoubles(std::pmr::memory_resource& mr,
                                   std::initializer_list<double> values) {
  luxir::api::Val val;
  auto& arr = val.kind.emplace<luxir::api::ArrDouble>();
  double* out = luxir::api::build::allocArray(arr.v, values.size(), mr);
  std::copy(values.begin(), values.end(), out);
  return val;
}

static luxir::api::Val makeInts(std::pmr::memory_resource& mr,
                                std::initializer_list<int64_t> values) {
  luxir::api::Val val;
  auto& arr = val.kind.emplace<luxir::api::ArrInt>();
  int64_t* out = luxir::api::build::allocArray(arr.v, values.size(), mr);
  std::copy(values.begin(), values.end(), out);
  return val;
}

static luxir::api::Val makeVals(std::pmr::memory_resource& mr,
                                std::initializer_list<luxir::api::Val> values) {
  luxir::api::Val val;
  auto& arr = val.kind.emplace<luxir::api::ArrVal>();
  luxir::api::Val* out = luxir::api::build::allocArray(arr.v, values.size(), mr);
  std::copy(values.begin(), values.end(), out);
  return val;
}

static void indexVal(Inverter& inverter, Inverter::IndexHandler& handler, const luxir::api::Val& val) {
  handler.index(inverter, val);
}

static void enableCosineOnVecSuffix(Collection& col, bool normalizeOnWrite = true) {
  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::COSINE;
  f.normalize_on_write = normalizeOnWrite;
  b.set(col);
}

// Index a multi-valued vector value (arr_vec) for the given doc.
static void indexMultiVec(Inverter& inverter, Inverter::IndexHandler& handler,
                          int32_t docid, std::vector<std::vector<float>> vecs) {
  std::pmr::monotonic_buffer_resource mr;
  inverter.setDoc(docid);
  luxir::api::Val v;
  auto& arr = v.kind.emplace<luxir::api::ArrVector>();
  auto* a = luxir::api::build::allocArray(arr.v, vecs.size(), mr);
  for (std::size_t i = 0; i < vecs.size(); i++) {
    auto& f32 = a[i].f32.emplace();
    float* fa = luxir::api::build::allocArray(f32.v, vecs[i].size(), mr);
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
  FieldReader fieldReader(seg.postingsReader());
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
  luxir::api::Val val;
  auto& f32 = val.kind.emplace<luxir::api::Vector>().f32.emplace();
  constexpr size_t DIMS = 256 * 1024;
  float* values = luxir::api::build::allocArray(f32.v, DIMS, mr);
  for (size_t i = 0; i < DIMS; i++) values[i] = (float)i;

  size_t extraBefore = inverter.extraRamBytes;
  inverter.setDoc(0);
  indexVal(inverter, handler, val);
  EXPECT_EQ(extraBefore, inverter.extraRamBytes);

  testIndex.flush();
  testIndex.initReader();
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fieldReader(seg.postingsReader());
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
  constexpr size_t VECTOR_VALUES =
      PostingsWriter::RAM_SPILL_BYTES / sizeof(float) + 512;
  std::string pathTemplate =
      (std::filesystem::temp_directory_path() / "luxir_vector_io_XXXXXX").string();
  ASSERT_NE(nullptr, ::mkdtemp(pathTemplate.data()));
  std::filesystem::path path(pathTemplate);

  {
    FSDirectory dir(path);
    IndexWriter iw(dir);

    auto submit = [&](bool commit) {
      std::pmr::monotonic_buffer_resource mr;
      luxir::api::UpdateRequest request;
      auto* docs = luxir::api::build::allocArray(request.docs, 1, mr);
      // Cross the delegating-file safety threshold so the poisoned backing
      // path is reached during the streamed write, before commit/finalize.
      CollectionHelper::convertDocToProto(
          flatdoc("vec_v", std::vector<float>(VECTOR_VALUES, 1.0f)), docs[0], mr);
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

// Stored fields and vectors release their indexing streams before the full-text
// flush reuses them. None spills on this tiny segment, so finalize can fold the
// three-stream high-water mark back into mandatory file 0.
TEST_F(VectorColTest, indexingStreamsReuseAndCollapse) {
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

  std::vector<Directory::FileInfo> files;
  testIndex.dir.listFiles(files);
  std::string dataPrefix = Postings::getIndexFileNamePrefix(segId) + "_";
  size_t dataFiles = std::count_if(files.begin(), files.end(), [&](const Directory::FileInfo& file) {
    return file.name.starts_with(dataPrefix);
  });
  EXPECT_EQ(1u, dataFiles);
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
  FieldReader fieldReader(seg.postingsReader());
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

TEST_F(VectorColTest, coercesNumericArrayArms) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("vec_v");
  std::pmr::monotonic_buffer_resource mr;

  inverter.setDoc(0);
  indexVal(inverter, handler, makeDoubles(mr, {1.5, 2.5, 3.5}));
  inverter.setDoc(1);
  indexVal(inverter, handler, makeInts(mr, {4, 5, 6}));
  inverter.setDoc(2);
  indexVal(inverter, handler,
           makeVals(mr, {coerce::scalarVal((int64_t)7),
                         coerce::scalarVal(8.5),
                         coerce::scalarVal(9.5f)}));

  testIndex.flush();
  testIndex.initReader();
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fields(seg.postingsReader());
  ASSERT_TRUE(fields.seek("vec_v"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  VectorReader reader(seg.postingsReader(), info);
  ASSERT_EQ(3, reader.docsWithValue());
  auto vec0 = reader.singleVectorAt(0);
  auto vec1 = reader.singleVectorAt(1);
  auto vec2 = reader.singleVectorAt(2);
  EXPECT_EQ((std::vector<float>{1.5f, 2.5f, 3.5f}),
            (std::vector<float>(vec0.begin(), vec0.end())));
  EXPECT_EQ((std::vector<float>{4, 5, 6}),
            (std::vector<float>(vec1.begin(), vec1.end())));
  EXPECT_EQ((std::vector<float>{7, 8.5f, 9.5f}),
            (std::vector<float>(vec2.begin(), vec2.end())));
}

TEST_F(VectorColTest, coercesMultiVectorJsonShapes) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("emb_vs");
  std::pmr::monotonic_buffer_resource mr;

  inverter.setDoc(0);
  indexVal(inverter, handler,
           makeVals(mr, {makeDoubles(mr, {1, 2}), makeInts(mr, {3, 4})}));
  inverter.setDoc(1);
  indexVal(inverter, handler, makeDoubles(mr, {5, 6}));
  inverter.setDoc(2);
  indexVal(inverter, handler, makeVals(mr, {}));

  testIndex.flush();
  testIndex.initReader();
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fields(seg.postingsReader());
  ASSERT_TRUE(fields.seek("emb_vs"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  VectorReader reader(seg.postingsReader(), info);
  EXPECT_EQ(2, reader.docsWithValue());
  EXPECT_EQ(3, reader.numVectors());
  auto [start, end] = reader.valueRange(1);
  ASSERT_EQ(1, end - start);
  auto vec = reader.vectorAtRank(start);
  EXPECT_EQ((std::vector<float>{5, 6}),
            (std::vector<float>(vec.begin(), vec.end())));
}

TEST_F(VectorColTest, rejectsInvalidCoercedVectorShapesAndSkipsNull) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& single = inverter.getIndexHandler("vec_v");
  auto& multi = inverter.getIndexHandler("emb_vs");
  std::pmr::monotonic_buffer_resource mr;

  EXPECT_THROW(indexVal(inverter, single,
                        makeVals(mr, {makeDoubles(mr, {1, 2})})), std::runtime_error);
  try {
    indexVal(inverter, single, makeVals(mr, {}));
    FAIL() << "expected empty vector to fail";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("empty vector"), std::string::npos) << e.what();
  }
  EXPECT_THROW(indexVal(inverter, single, coerce::scalarVal((int64_t)1)), std::runtime_error);
  EXPECT_THROW(indexVal(inverter, single, coerce::scalarVal(std::string_view("1,2"))),
               std::runtime_error);
  EXPECT_THROW(indexVal(inverter, single,
                        makeVals(mr, {coerce::scalarVal((int64_t)1),
                                      coerce::scalarVal(std::string_view("x"))})),
               std::runtime_error);

  luxir::api::Val unset;
  luxir::api::Val nullVal;
  nullVal.kind = google::protobuf::NullValue::NULL_VALUE;
  EXPECT_NO_THROW(indexVal(inverter, single, unset));
  EXPECT_NO_THROW(indexVal(inverter, single, nullVal));
  EXPECT_NO_THROW(indexVal(inverter, multi, makeVals(mr, {})));

  try {
    indexVal(inverter, multi, makeVals(mr, {makeVals(mr, {})}));
    FAIL() << "expected empty contained vector to fail";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("vector 0"), std::string::npos) << e.what();
    EXPECT_NE(std::string(e.what()).find("empty vector"), std::string::npos) << e.what();
  }
}

TEST_F(VectorColTest, coercedValidationPrecedesStreamingWrite) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto& handler = inverter.getIndexHandler("emb_vs");
  auto* vectorHandler = dynamic_cast<handler::VectorHandler*>(&handler);
  ASSERT_NE(nullptr, vectorHandler);
  std::pmr::monotonic_buffer_resource mr;

  inverter.setDoc(0);
  EXPECT_THROW(indexVal(inverter, handler,
                        makeVals(mr, {makeDoubles(mr, {1, 2}),
                                      makeDoubles(mr, {3, 4, 5})})),
               std::runtime_error);
  EXPECT_EQ(0, vectorHandler->dims());

  inverter.setDoc(1);
  EXPECT_NO_THROW(indexVal(inverter, handler,
                           makeVals(mr, {makeDoubles(mr, {1, 2, 3}),
                                         makeInts(mr, {4, 5, 6})})));
  EXPECT_EQ(3, vectorHandler->dims());

  testIndex.flush();
  testIndex.initReader();
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fields(seg.postingsReader());
  ASSERT_TRUE(fields.seek("emb_vs"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  VectorReader reader(seg.postingsReader(), info);
  EXPECT_EQ(1, reader.docsWithValue());
  EXPECT_EQ(2, reader.numVectors());
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
  FieldReader fieldReader(seg.postingsReader());
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
  FieldReader fieldReader(seg.postingsReader());
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
  auto wrong = makeDoubles(mr, {1, 2, 3});  // coerced 3 dims vs declared 4
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
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::COSINE;
  b.set(h.collection());

  // This pins the default normalized column storage for cosine fields. A
  // coerced double array follows the same path as a typed float vector.
  Doc doc = flatdoc("id", std::string("a"), "vec_v", std::vector<double>{3.0, 4.0});
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
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::COSINE;
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
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.metric = luxir::api::VectorMetric::COSINE;
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

TEST_F(VectorColTest, rejectsDoubleValuesOutsideFiniteFloat32Range) {
  TestIndex testIndex;
  auto& inverter = testIndex.getInverter();
  auto strictType = std::make_shared<VectorFieldType>("strict_v", /*dims=*/1);
  handler::VectorHandler vh(inverter, "strict_v", strictType);
  std::pmr::monotonic_buffer_resource mr;

  EXPECT_THROW(indexVal(inverter, vh,
                        makeDoubles(mr, {std::numeric_limits<double>::max()})),
               std::runtime_error);
  EXPECT_THROW(indexVal(inverter, vh,
                        makeDoubles(mr, {std::numeric_limits<double>::infinity()})),
               std::runtime_error);
  EXPECT_THROW(indexVal(inverter, vh,
                        makeDoubles(mr, {std::numeric_limits<double>::quiet_NaN()})),
               std::runtime_error);
}

// Schema round-trip: toProto/fromProto preserves VECTOR field with dims.
TEST_F(VectorColTest, schemaProtoRoundTrip) {
  SchemaBuilder b;
  auto& f = b.field("embedding");
  f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.dims = 384;
  f.metric = luxir::api::VectorMetric::COSINE;

  auto schema = b.build();
  auto* vft = dynamic_cast<VectorFieldType*>(schema->getFieldTypePtr("embedding"));
  ASSERT_NE(nullptr, vft);
  EXPECT_EQ(384, vft->dims());
  EXPECT_EQ(VectorFieldType::METRIC_COSINE, vft->metric());
  EXPECT_TRUE(vft->normalizeOnWrite());
  EXPECT_TRUE(vft->hasColumn());
  EXPECT_TRUE(vft->isSet(FieldType::FIXED_SIZE));

  luxir::api::SchemaDef outDef;
  std::pmr::monotonic_buffer_resource outMr;
  schema->toProto(&outDef, outMr);
  const auto* fd = outDef.fields.at("embedding").operator->();
  ASSERT_NE(nullptr, fd);
  ASSERT_TRUE(fd->type.has_value());
  EXPECT_EQ(luxir::api::FieldDef_::FieldClass::VECTOR, *fd->type);
  ASSERT_TRUE(fd->dims.has_value());
  EXPECT_EQ(384, *fd->dims);
  ASSERT_TRUE(fd->metric.has_value());
  EXPECT_EQ(luxir::api::VectorMetric::COSINE, *fd->metric);
  // toProto emits the authored sparse def: normalize_on_write was never set
  // (the on-write default is derived from COSINE), so it round-trips as absent.
  EXPECT_FALSE(fd->normalize_on_write.has_value());
}
