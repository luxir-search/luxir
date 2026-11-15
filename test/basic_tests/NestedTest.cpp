// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/NestedFixture.h"
#include "luxir/reader/OwnerMap.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr const char* VARIANTS_SCHEMA = R"({"fields": {
  "title": "text",
  "variants": {"type": "nested", "fields": {
    "color": "string", "price": {"type": "int", "index": "range"}, "note": "text"}}
}})";

class NestedTest : public NestedFixture {};

TEST_F(NestedTest, elementsGetTheirOwnRowSpace) {
  schema(VARIANTS_SCHEMA);
  auto r = index({
    R"({"id": "A", "title": "shirt", "variants": [{"color": "red", "price": 10}, {"color": "blue", "price": 12}]})",
    R"({"id": "B", "title": "hat", "variants": []})",
    R"({"id": "C", "title": "sock", "variants": [{"color": "red", "price": 7}]})",
    R"({"id": "D", "title": "bag"})",
  });
  ASSERT_TRUE(r.success) << r.error_message << (r.errors.empty() ? "" : r.errors[0].error_message);

  auto reader = helper().getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& seg = reader->segments()[0];
  EXPECT_EQ(4, seg.maxDoc());
  ASSERT_TRUE(seg.postingsReader().hasScope("variants"));
  auto& dir = helper().getIndexWriter()->dir;
  auto scope = PostingsReader::createScope(dir, seg.segInfo.seg_id, seg.postingsReader().creatorEpoch(), "variants");
  ASSERT_NE(nullptr, scope);
  EXPECT_EQ(3, scope->maxDoc());
  OwnerMap owners(*scope, 4);
  EXPECT_EQ(std::make_pair(0, 2), owners.range(0));
  EXPECT_EQ(std::make_pair(2, 2), owners.range(1));
  EXPECT_EQ(std::make_pair(2, 3), owners.range(2));
  EXPECT_EQ(std::make_pair(3, 3), owners.range(3));
  EXPECT_EQ(0, owners.owner(1));
  EXPECT_EQ(2, owners.owner(2));
  EXPECT_NE(nullptr, scope->fieldInfo("color"));
  EXPECT_EQ(nullptr, seg.postingsReader().fieldInfo("color"));
}

TEST_F(NestedTest, schemaSourceRoundTripsNestedFields) {
  std::pmr::monotonic_buffer_resource mr;
  api::SchemaDef def;
  std::string json = VARIANTS_SCHEMA;
  ASSERT_TRUE(api::read_json(def, json, mr));
  auto compiled = Schema::fromProto(def);
  api::SchemaDef echoed;
  compiled->toProto(&echoed, mr);
  std::string out;
  ASSERT_TRUE(api::write_json(echoed, out));
  EXPECT_NE(std::string::npos, out.find(R"("variants":{"type":"nested","fields":{"color")")) << out;
  auto again = Schema::fromProto(echoed);
  ASSERT_NE(nullptr, again->scope("variants"));
  EXPECT_NE(nullptr, (*again->scope("variants"))->getFieldTypePtr("price"));
}

TEST_F(NestedTest, schemaValidation) {
  auto bad = [&](std::string json, std::string_view message) {
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef def;
    ASSERT_TRUE(api::read_json(def, json, mr)) << json;
    try {
      Schema::fromProto(def);
      FAIL() << "accepted " << json;
    } catch (const SchemaError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find(message)) << e.what();
    }
  };
  bad(R"({"fields": {"v": {"type": "nested"}}})", "requires fields");
  bad(R"({"fields": {"v": {"type": "string", "fields": {"a": "int"}}}})", "only valid for nested");
  bad(R"({"fields": {"v": {"type": "nested", "fields": {"w": {"type": "nested", "fields": {"a": "int"}}}}}})",
      "one nesting level");
  bad(R"({"fields": {"v": {"type": "nested", "column": true, "fields": {"a": "int"}}}})", "accepts only");
  bad(R"({"fields": {"v": {"type": "nested", "fields": {"a": "id"}}}})", "not valid inside nested");
  bad(R"({"templates": {"_n": {"type": "nested", "fields": {"a": "int"}}}})", "cannot be nested");
}

void indexVariants(NestedFixture& f) {
  auto r = f.index({
    R"({"id": "A", "title": "shirt", "variants": [{"color": "red", "price": 10}, {"color": "blue", "price": 12}]})",
    R"({"id": "B", "title": "hat", "variants": [{"color": "red", "price": 12}, {"color": "blue", "price": 10}]})",
    R"({"id": "C", "title": "sock", "variants": [{"color": "red", "price": 7}]})",
    R"({"id": "D", "title": "bag", "variants": []})",
    R"({"id": "E", "title": "cap"})",
  });
  ASSERT_TRUE(r.success) << r.error_message;
}

std::string topDocs(std::string query, std::string extra = "") {
  return R"({"ops": {"q": {"top_docs": {"query": )" + query + R"(, "limit": -1, "fields": ["id"])" + extra + "}}}}";
}

TEST_F(NestedTest, toParentCorrelatesClausesOnOneElement) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  // red AND price 10 on the same element: only A.
  auto q = R"({"to_parent": {"path": "variants", "query": {"boolean": {"required": [
      {"match": {"variants.color": "red"}}, {"range": {"field": "variants.price", "gte": 10, "lte": 10}}]}}}})";
  EXPECT_EQ(std::vector<std::string>({"A"}), ids(topDocs(q)));
  // Two to_parent clauses may match different elements: A and B.
  auto q2 = R"({"boolean": {"required": [
      {"to_parent": {"path": "variants", "query": {"match": {"variants.color": "red"}}}},
      {"to_parent": {"path": "variants", "query": {"range": {"field": "variants.price", "gte": 10, "lte": 10}}}}]}})";
  auto got = ids(topDocs(q2));
  std::sort(got.begin(), got.end());
  EXPECT_EQ(std::vector<std::string>({"A", "B"}), got);
  // Inside must_not.
  auto q3 = R"({"boolean": {"prohibited": [{"to_parent": {"path": "variants", "query": {"match": {"variants.color": "blue"}}}}]}})";
  got = ids(topDocs(q3));
  std::sort(got.begin(), got.end());
  EXPECT_EQ(std::vector<std::string>({"C", "D", "E"}), got);
}

TEST_F(NestedTest, scopeErrors) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  EXPECT_NE(std::string::npos, error(topDocs(R"({"match": {"variants.color": "red"}})")).find("to_parent"));
  EXPECT_NE(std::string::npos, error(topDocs(
      R"({"to_parent": {"path": "variants", "query": {"match": {"color": "red"}}}})")).find("variants.<field>"));
  EXPECT_NE(std::string::npos, error(topDocs(
      R"({"to_parent": {"path": "title", "query": {"match": {"variants.color": "red"}}}})")).find("not a nested"));
  EXPECT_NE(std::string::npos, error(topDocs(
      R"({"to_parent": {"path": "variants", "score": "median", "query": {"match": {"variants.color": "red"}}}})")).find("score"));
}

TEST_F(NestedTest, toParentCountBounds) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  auto all = [](std::string bounds) {
    return R"({"to_parent": {"path": "variants", "query": {"range": {"field": "variants.price", "gte": 0}})" + bounds + "}}";
  };
  auto sorted = [&](std::string q) { auto v = ids(topDocs(q)); std::sort(v.begin(), v.end()); return v; };
  EXPECT_EQ(std::vector<std::string>({"A", "B", "C"}), sorted(all("")));
  EXPECT_EQ(std::vector<std::string>({"A", "B"}), sorted(all(R"(, "min_children": 2)")));
  EXPECT_EQ(std::vector<std::string>({"C"}), sorted(all(R"(, "max_children": 1)")));
}

TEST_F(NestedTest, retrievalReturnsTheSubmittedStructure) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  auto json = search(R"({"ops": {"q": {"top_docs": {"query": {"match": {"id": "A"}},
      "fields": ["id", "variants", "variants.color"], "document_format": "rows"}}}})");
  EXPECT_NE(std::string::npos, json.find(R"("variants":[{"color":"red","price":10},{"color":"blue","price":12}])")) << json;
  EXPECT_NE(std::string::npos, json.find(R"("variants.color":["red","blue"])")) << json;
  json = search(R"({"ops": {"q": {"top_docs": {"query": {"match": {"id": "D"}}, "fields": ["variants"], "document_format": "rows"}}}})");
  EXPECT_NE(std::string::npos, json.find(R"("variants":[])")) << json;
  // Every retrievable field by default, including the nested source.
  json = search(R"({"ops": {"q": {"top_docs": {"query": {"match": {"id": "C"}}, "document_format": "rows"}}}})");
  EXPECT_NE(std::string::npos, json.find(R"([{"color":"red","price":7}])")) << json;
}

TEST_F(NestedTest, toParentScoreReducers) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  auto reduced = [&](std::string mode) {
    std::string q = R"({"to_parent": {"path": "variants", "score": ")" + mode + R"(", "query": {"boolean": {"optional": [
        {"constant_score": {"query": {"match": {"variants.color": "red"}}, "score": 2}},
        {"constant_score": {"query": {"match": {"variants.color": "blue"}}, "score": 1}}]}}}})";
    return scores(R"({"ops": {"q": {"top_docs": {"query": )" + q +
                  R"(, "limit": -1, "fields": ["id", "_score_"], "get_scores": true, "document_format": "rows"}}}})");
  };
  using M = std::map<std::string, double>;
  EXPECT_EQ((M{{"A", 2}, {"B", 2}, {"C", 2}}), reduced("max"));
  EXPECT_EQ((M{{"A", 1}, {"B", 1}, {"C", 2}}), reduced("min"));
  EXPECT_EQ((M{{"A", 3}, {"B", 3}, {"C", 2}}), reduced("sum"));
  EXPECT_EQ((M{{"A", 1.5}, {"B", 1.5}, {"C", 2}}), reduced("avg"));
  EXPECT_EQ((M{{"A", 0}, {"B", 0}, {"C", 0}}), reduced("none"));
}

TEST_F(NestedTest, deletesOverwritesAndMergesKeepElementsWithTheirOwner) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  ASSERT_TRUE(index({R"({"id": "F", "variants": [{"color": "green", "price": 1}]})"}).success);
  auto red = topDocs(R"({"to_parent": {"path": "variants", "query": {"match": {"variants.color": "red"}}}})");
  auto green = topDocs(R"({"to_parent": {"path": "variants", "query": {"match": {"variants.color": "green"}}}})");
  auto sorted = [&](std::string q) { auto v = ids(q); std::sort(v.begin(), v.end()); return v; };
  EXPECT_EQ(std::vector<std::string>({"A", "B", "C"}), sorted(red));

  // Delete one owner, overwrite another with different elements.
  helper().deleteById("A", UpdateMessage::COMMIT);
  ASSERT_TRUE(index({R"({"id": "B", "title": "hat", "variants": [{"color": "green", "price": 3}]})"}, true, true).success);
  EXPECT_EQ(std::vector<std::string>({"C"}), sorted(red));
  EXPECT_EQ(std::vector<std::string>({"B", "F"}), sorted(green));

  // Merge everything; element runs follow their surviving owners.
  CollectionHelper h = helper();
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(h.submit(merge).success);
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_EQ(std::vector<std::string>({"C"}), sorted(red));
  EXPECT_EQ(std::vector<std::string>({"B", "F"}), sorted(green));
  auto* scope = reader->scope("variants");
  ASSERT_NE(nullptr, scope);
  EXPECT_EQ(3, scope->segments()[0].maxDoc());  // C, B (new), F
  auto json = search(R"({"ops": {"q": {"top_docs": {"query": {"match": {"id": "B"}}, "fields": ["variants.color"], "document_format": "rows"}}}})");
  EXPECT_NE(std::string::npos, json.find(R"(["green"])")) << json;
}

TEST_F(NestedTest, aFailedElementRollsBackTheWholeDocument) {
  schema(VARIANTS_SCHEMA);
  auto r = index({
    R"({"id": "A", "variants": [{"color": "red", "price": 1}]})",
    R"({"id": "B", "variants": [{"color": "red", "price": 2}, {"color": "red", "price": "not a number"}]})",
    R"({"id": "C", "variants": [{"color": "red", "unknown": 1}]})",
    R"({"id": "D", "variants": [5]})",
    R"({"id": "E", "variants": {"color": "red"}})",
    R"({"id": "G", "variants": [null, {"color": "red"}]})",
  });
  EXPECT_EQ(3, r.total_errors);
  auto got = ids(topDocs(R"({"to_parent": {"path": "variants", "query": {"match": {"variants.color": "red"}}}})"));
  std::sort(got.begin(), got.end());
  EXPECT_EQ(std::vector<std::string>({"A", "E", "G"}), got);
}

constexpr const char* CHUNKS_SCHEMA = R"({"fields": {
  "title": "text",
  "chunks": {"type": "nested", "fields": {
    "text": "text", "embedding": {"type": "vector", "dims": 2, "metric": "l2"}}}
}})";

void indexChunks(NestedFixture& f) {
  auto r = f.index({
    R"({"id": "X", "title": "one", "chunks": [{"text": "alpha", "embedding": [0, 0]}, {"text": "alpha again", "embedding": [0, 0.1]}, {"text": "far", "embedding": [9, 9]}]})",
    R"({"id": "Y", "title": "two", "chunks": [{"text": "beta", "embedding": [0, 0.5]}]})",
    R"({"id": "Z", "title": "three", "chunks": [{"text": "gamma", "embedding": [5, 5]}]})",
  });
  ASSERT_TRUE(r.success) << r.error_message << (r.errors.empty() ? "" : r.errors[0].error_message);
}

TEST_F(NestedTest, knnInsideToParentCountsOwnersWithOwnerUnit) {
  schema(CHUNKS_SCHEMA);
  indexChunks(*this);
  auto knn = [](std::string unit) {
    return R"({"to_parent": {"path": "chunks", "query": {"knn": {"field": "chunks.embedding", "query": [0, 0], "k": 2, "exact": true)" +
        unit + "}}}}";
  };
  // The two nearest elements both belong to X.
  EXPECT_EQ(std::vector<std::string>({"X"}), ids(topDocs(knn(""))));
  // k distinct owners, ranked by their best element.
  EXPECT_EQ(std::vector<std::string>({"X", "Y"}), ids(topDocs(knn(R"(, "unit": "owner")"))));
  // Owner filters apply before selection: excluding X leaves Y and Z.
  auto filtered = R"({"ops": {"q": {"top_docs": {"query": )" + knn(R"(, "unit": "owner")") +
      R"(, "filter": [{"boolean": {"prohibited": [{"match": {"id": "X"}}]}}], "limit": -1, "fields": ["id"]}}}})";
  EXPECT_EQ(std::vector<std::string>({"Y", "Z"}), ids(filtered));
}

TEST_F(NestedTest, phraseAndBm25InElementText) {
  schema(CHUNKS_SCHEMA);
  indexChunks(*this);
  auto phrase = R"({"to_parent": {"path": "chunks", "query": {"phrase": {"field": "chunks.text", "text": "alpha again"}}}})";
  EXPECT_EQ(std::vector<std::string>({"X"}), ids(topDocs(phrase)));
  // The phrase must occur within one element, not across element boundaries.
  auto across = R"({"to_parent": {"path": "chunks", "query": {"phrase": {"field": "chunks.text", "text": "again far"}}}})";
  EXPECT_TRUE(ids(topDocs(across)).empty());
}

TEST_F(NestedTest, domainToChildrenRunsOperationsOverElements) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  // Elements of the products matching the list, faceted by color, and the
  // cheapest red element per product listed in element scope.
  auto json = search(R"({"ops": {"q": {"top_docs": {"query": {"match": {"title": "shirt hat"}}, "limit": 10, "fields": ["id"],
      "ops": {
        "colors": {"domain": {"to_children": "variants"}, "field_facet": {"field": "variants.color"}},
        "reds": {"domain": {"to_children": "variants", "filter": [{"match": {"variants.color": "red"}}]},
                 "top_docs": {"limit": 10, "fields": ["variants.price", "root.id"], "sort": [{"expr": "variants.price"}],
                              "document_format": "rows"}}}}}}})");
  EXPECT_NE(std::string::npos, json.find(R"("red")")) << json;
  EXPECT_NE(std::string::npos, json.find(R"("blue")")) << json;
  EXPECT_NE(std::string::npos, json.find(R"({"variants.price":10,"root.id":"A"})")) << json;
  EXPECT_EQ(std::string::npos, json.find(R"("variants.price":7)")) << json;  // C is not in the list
}

TEST_F(NestedTest, domainToParentProjectsBackToOwners) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  auto json = search(R"({"ops": {"v": {"domain": {"to_children": "variants", "filter": [{"range": {"field": "variants.price", "lte": 10}}]},
      "top_docs": {"limit": 0, "count_limit": -1, "ops": {
        "owners": {"domain": {"to_parent": true}, "top_docs": {"limit": 10, "count_limit": -1, "fields": ["id"], "document_format": "rows"}}}}}}})");
  // Elements with price <= 10: A/red, B/blue, C/red; owners A, B, C.
  EXPECT_NE(std::string::npos, json.find(R"("found":3)")) << json;
  EXPECT_NE(std::string::npos, json.find(R"({"id":"C"})")) << json;
}

TEST_F(NestedTest, topElementsDiversifiedByOwner) {
  schema(CHUNKS_SCHEMA);
  indexChunks(*this);
  // Top chunks, at most one per document, with the owner's id.
  auto json = search(R"({"ops": {"chunks": {"domain": {"to_children": "chunks"}, "top_docs": {
      "query": {"knn": {"field": "chunks.embedding", "query": [0, 0], "k": 10, "exact": true}},
      "limit": 10, "diversify": {"owner": "root", "max_per_key": 1},
      "fields": ["chunks.text", "root.id"], "document_format": "rows"}}}})");
  EXPECT_NE(std::string::npos, json.find(R"([{"chunks.text":"alpha","root.id":"X"},{"chunks.text":"beta","root.id":"Y"},{"chunks.text":"gamma","root.id":"Z"}])")) << json;
  // Collapse by owner: one group per document.
  json = search(R"({"ops": {"chunks": {"domain": {"to_children": "chunks"}, "top_docs": {
      "query": {"knn": {"field": "chunks.embedding", "query": [0, 0], "k": 10, "exact": true}},
      "limit": 10, "collapse": {"owner": "root"}, "count_limit": -1,
      "fields": ["chunks.text"], "document_format": "rows"}}}})");
  EXPECT_NE(std::string::npos, json.find(R"("found":3,"found_docs":5)")) << json;
  // Owner keys outside a nested scope are rejected.
  EXPECT_NE(std::string::npos, error(R"({"ops": {"q": {"top_docs": {"diversify": {"owner": "root", "max_per_key": 1}}}}})")
            .find("nested path"));
}

TEST_F(NestedTest, sortByElementArrayReducers) {
  schema(VARIANTS_SCHEMA);
  indexVariants(*this);
  ASSERT_TRUE(index({R"({"id": "G", "variants": [{"color": "red", "price": 3}, {"color": "red"}, {"color": "red", "price": 30}]})"}).success);
  auto sorted = [&](std::string expr, std::string dir) {
    return ids(R"({"ops": {"q": {"top_docs": {"limit": -1, "fields": ["id"], "sort": [{"expr": ")" + expr +
               R"(", "dir": ")" + dir + R"("}, {"expr": "id"}]}}}})");
  };
  // Owners without elements (D, E) sort last.
  EXPECT_EQ(std::vector<std::string>({"G", "C", "A", "B", "D", "E"}), sorted("min(variants.price)", "asc"));
  EXPECT_EQ(std::vector<std::string>({"G", "A", "B", "C", "D", "E"}), sorted("max(variants.price)", "desc"));
  EXPECT_EQ(std::vector<std::string>({"A", "B", "G", "C", "D", "E"}), sorted("count(variants.price)", "desc"));
  // A bare element field needs a reducer.
  EXPECT_NE(std::string::npos, error(R"({"ops": {"q": {"top_docs": {"sort": [{"expr": "variants.price"}]}}}})").find("reducer"));
}

TEST_F(NestedTest, rowOpsReturnEachRowsMatchingElements) {
  schema(CHUNKS_SCHEMA);
  indexChunks(*this);
  // Documents by their best "alpha" chunk, with the chunks that clause admitted.
  auto json = search(R"({"ops": {"q": {"top_docs": {
      "query": {"to_parent": {"path": "chunks", "name": "hits", "query": {"match": {"chunks.text": "alpha"}}}},
      "limit": 10, "fields": ["id"], "document_format": "rows",
      "row_ops": {
        "admitted": {"domain": {"from_match": "hits"}, "top_docs": {"limit": 3, "fields": ["chunks.text"], "document_format": "rows"}},
        "all": {"domain": {"to_children": "chunks"}, "top_docs": {"limit": 1, "count_limit": -1, "fields": ["chunks.text"], "document_format": "rows"}}
      }}}}})");
  EXPECT_NE(std::string::npos, json.find(R"("id":"X")")) << json;
  EXPECT_NE(std::string::npos, json.find(R"("admitted":{"docs":[{"chunks.text":"alpha"},{"chunks.text":"alpha again"}],"row_count":2})")) << json;
  EXPECT_NE(std::string::npos, json.find(R"("all":{"found":3,)")) << json;
  EXPECT_NE(std::string::npos, error(R"({"ops": {"q": {"top_docs": {"row_ops": {"r": {"domain": {"from_match": "nope"}, "top_docs": {}}}}}}})")
            .find("no to_parent clause named"));
}

TEST_F(NestedTest, aPathAddedLaterSeesOlderSegmentsAsElementless) {
  schema(R"({"fields": {"title": "text"}})");
  ASSERT_TRUE(index({R"({"id": "old", "title": "before"})"}).success);
  schema(VARIANTS_SCHEMA);
  ASSERT_TRUE(index({R"({"id": "new", "title": "after", "variants": [{"color": "red", "price": 1}]})"}).success);
  auto red = topDocs(R"({"to_parent": {"path": "variants", "query": {"match": {"variants.color": "red"}}}})");
  EXPECT_EQ(std::vector<std::string>({"new"}), ids(red));
  auto sorted = R"J({"ops": {"q": {"top_docs": {"limit": -1, "fields": ["id"], "sort": [{"expr": "count(variants.price)", "dir": "desc"}]}}}})J";
  EXPECT_EQ(std::vector<std::string>({"new", "old"}), ids(sorted));
  CollectionHelper h = helper();
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(h.submit(merge).success);
  EXPECT_EQ(1u, h.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  EXPECT_EQ(std::vector<std::string>({"new"}), ids(red));
  EXPECT_EQ(std::vector<std::string>({"new", "old"}), ids(sorted));
}

}  // namespace
