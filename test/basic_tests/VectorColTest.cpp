#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "solux/index/handler/VectorHandler.h"
#include "solux/reader/VectorReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/schema/FieldType.h"
#include "solux/schema/Schema.h"
#include "protos/solux_types.pb.h"

using namespace solux;
using namespace solux::test;

class VectorColTest : public SoluxTest {};

// Build a proto::Val containing a single Vector{f32}.
static proto::Val makeVec(std::initializer_list<float> floats) {
  proto::Val v;
  auto* f32 = v.mutable_vec()->mutable_f32();
  for (float f : floats) f32->add_v(f);
  return v;
}

// Index a multi-valued vector value (arr_vec) for the given doc.
static void indexMultiVec(Inverter& inverter, Inverter::IndexHandler& handler,
                          int32_t docid, std::vector<std::vector<float>> vecs) {
  inverter.setDoc(docid);
  proto::Val v;
  auto* arr = v.mutable_arr_vec();
  for (auto& vec : vecs) {
    auto* f32 = arr->add_v()->mutable_f32();
    for (float f : vec) f32->add_v(f);
  }
  handler.index(inverter, v);
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

  for (size_t doc = 0; doc < expected.size(); doc++) {
    inverter.setDoc((int32_t)doc);
    auto val = makeVec({expected[doc][0], expected[doc][1], expected[doc][2], expected[doc][3]});
    handler.index(inverter, val);
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
    inverter.setDoc((int32_t)doc);
    proto::Val v;
    auto* arr = v.mutable_arr_vec();
    for (auto& vec : expected[doc]) {
      auto* f32 = arr->add_v()->mutable_f32();
      for (float f : vec) f32->add_v(f);
    }
    handler.index(inverter, v);
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
    inverter.setDoc(dv.docId);
    proto::Val v;
    auto* arr = v.mutable_arr_vec();
    for (auto& vec : dv.vecs) {
      auto* f32 = arr->add_v()->mutable_f32();
      for (float f : vec) f32->add_v(f);
    }
    handler.index(inverter, v);
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

  inverter.setDoc(0);
  auto v0 = makeVec({1, 2, 3, 4, 5, 6});
  handler.index(inverter, v0);

  inverter.setDoc(1);
  auto v1 = makeVec({7, 8, 9, 10, 11});  // wrong dims
  EXPECT_THROW(handler.index(inverter, v1), std::runtime_error);

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

  inverter.setDoc(0);
  auto wrong = makeVec({1, 2, 3});  // 3 dims vs declared 4
  EXPECT_THROW(vh.index(inverter, wrong), std::runtime_error);

  inverter.setDoc(1);
  auto right = makeVec({1, 2, 3, 4});
  EXPECT_NO_THROW(vh.index(inverter, right));
}

// gRPC round trip (single-valued): index via CollectionHelper, retrieve via LocalReq.
TEST_F(VectorColTest, grpcSingleFieldsRoundTrip) {
  CollectionHelper h("main");
  h.clear();

  Doc doc1 = flatdoc("id", std::string("a"), "vec_v", std::vector<float>{1.0f, 2.0f, 3.0f});
  Doc doc2 = flatdoc("id", std::string("b"), "vec_v", std::vector<float>{4.5f, -5.5f, 6.5f});
  h.index(doc1);
  h.index(doc2, UpdateMessage::COMMIT);

  auto* req = LocalReq::create(h.getSearchEngine());
  req->collection("main")
     .allQuery()
     .fields({"id", "vec_v"})
     .limit(10)
     .execute();

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

  req->done();
}

// gRPC round trip (multi-valued): exercises the loadVectorColForSegmentMulti path.
TEST_F(VectorColTest, grpcMultiFieldsRoundTrip) {
  CollectionHelper h("main");
  h.clear();

  std::vector<std::vector<float>> aVecs = {{1, 2}, {3, 4}, {5, 6}};
  std::vector<std::vector<float>> bVecs = {{7, 8}};
  Doc doc1 = flatdoc("id", std::string("a"), "emb_vs", aVecs);
  Doc doc2 = flatdoc("id", std::string("b"), "emb_vs", bVecs);
  h.index(doc1);
  h.index(doc2, UpdateMessage::COMMIT);

  auto* req = LocalReq::create(h.getSearchEngine());
  req->collection("main")
     .allQuery()
     .fields({"id", "emb_vs"})
     .limit(10)
     .execute();

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

  req->done();
}

// Schema round-trip: toProto/fromProto preserves VECTOR field with dims.
TEST_F(VectorColTest, schemaProtoRoundTrip) {
  proto::SchemaDef def;
  auto* f = def.add_fields();
  f->set_name("embedding");
  f->set_field_class(proto::FieldDef::VECTOR);
  f->set_column_stored(true);
  f->mutable_vector()->set_dims(384);

  auto schema = Schema::fromProto(def);
  auto it = schema->getFieldType("embedding");
  ASSERT_NE(it, schema->end());
  auto* vft = dynamic_cast<VectorFieldType*>(it->second.get());
  ASSERT_NE(nullptr, vft);
  EXPECT_EQ(384, vft->dims());
  EXPECT_TRUE(vft->hasColumn());
  EXPECT_TRUE(vft->isSet(FieldType::FIXED_SIZE));

  proto::SchemaDef outDef;
  schema->toProto(&outDef);
  bool found = false;
  for (int i = 0; i < outDef.fields_size(); i++) {
    if (outDef.fields(i).name() == "embedding") {
      EXPECT_EQ(proto::FieldDef::VECTOR, outDef.fields(i).field_class());
      EXPECT_EQ(384, outDef.fields(i).vector().dims());
      found = true;
    }
  }
  EXPECT_TRUE(found);
}
