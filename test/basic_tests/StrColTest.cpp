// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "luxir/index/handler/StrColHandler.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/FieldReader.h"
#include <vector>

using namespace luxir;
using namespace luxir::test;

class StrColTest : public LuxirTest {
protected:

};


TEST_F(StrColTest, basic) {
  {
    // single doc
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(0, "mystring");
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // multiple docs
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(0, "ccc");  // ord 3
    f.add(1, "bbb");  // ord 2
    f.add(2, "aaa");  // ord 1
    f.add(3, "bbb");  // ord 2
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(2, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(3, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // sparse docs
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(5, "ccc");  // ord 3
    f.add(500, "bbb");  // ord 2
    f.add(500000, "aaa");  // ord 1
    f.add(5000000, "bbb");  // ord 2
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(5, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(500, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(500000, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(5000000, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }
}


TEST_F(StrColTest, basicMerge) {
  // dense merge
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(0, "ccc");  // global ord 3
    f.add(1, "bbb");  // global ord 2
    testIndex.flush();
    f.startIndexing();
    f.add(0, "aaa");  // global ord 1
    f.add(1, "bbb");  // global ord 2
    testIndex.flush();

    // TODO: force reopen of IndexReader since that is what mergeSegments uses?
    testIndex.iw->mergeSegments();

    f.startReading();

    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(2, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(3, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }


  // sparse merge
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(10, "ccc");      // global ord 3
    f.add(100000, "bbb");  // global ord 2
    testIndex.flush();
    f.startIndexing();
    f.add(7, "aaa");      // global ord 1
    f.add(70000, "bbb");  // global ord 2
    testIndex.flush();

    // TODO: force reopen of IndexReader since that is what mergeSegments uses?
    testIndex.iw->mergeSegments();

    f.startReading();

    ASSERT_EQ(10, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(100000, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(100000+1+7, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(100000+1+70000, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

}


TEST_F(StrColTest, multiValued) {

  {
    // single doc, single valued (but multi-valued field)
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.add(0, "mystring");
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // single doc, multi-valued, dense
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(0, {"b", "a"});
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    std::vector<int64_t> ords;
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l));
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // single doc, multi-valued, sparse (designed to trigger a bug
    // where inverter would think column was dense because numValues==numDocs
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(1, {"b", "a"});
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(1, f.nextDoc());
    std::vector<int64_t> ords;
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l));
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // multiple docs, multi-valued
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(5, {"b", "a"});
    f.addStrings(10, {"c", "c"});  // handle duplicates (or throw an error)
    f.addStrings(15, {"c", "b", "a"});
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(5, f.nextDoc());
    std::vector<int64_t> ords;
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l));
    ASSERT_EQ(10, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(3l));
    ASSERT_EQ(15, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l, 3l));
    ASSERT_EQ(-1, f.nextDoc());
  }

  // multi-valued dense merge, with one segment being single-valued
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(0, {"a"});
    f.addStrings(1, {"c"});
    testIndex.flush();
    f.startIndexing();
    f.addStrings(0, {"c", "b", "a"});
    f.addStrings(1, {"b"});
    testIndex.flush();

    testIndex.iw->mergeSegments();
    f.startReading();
    std::vector<int64_t> ords;
    ASSERT_EQ(0, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l));
    ASSERT_EQ(1, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(3l));
    ASSERT_EQ(2, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l, 3l));
    ASSERT_EQ(3, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(2l));
    ASSERT_EQ(-1, f.nextDoc());
  }
}

TEST_F(StrColTest, deleteAndMerge) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_s");
  
  // Add documents in first segment
  f.startIndexing();
  f.add(5, "hello");    // will be ord 1
  f.add(7, "world");    // will be ord 2  
  f.add(11, "test");    // will be ord 3
  testIndex.deleteDoc(7);  // delete the middle document
  testIndex.flush();
  
  // Add document in second segment
  f.startIndexing();
  f.add(0, "apple");    // will be doc10 after merge and term will be ord 1tes
  testIndex.flush();
  
  // Merge segments - this should trigger the merge bug
  testIndex.iw->mergeSegments();
  

  f.startReading();
  ASSERT_EQ(5, f.nextDoc());
  ASSERT_EQ(2, f.ord());  // "hello" 
  ASSERT_EQ(10, f.nextDoc());
  ASSERT_EQ(3, f.ord());  // "test"
  ASSERT_EQ(11, f.nextDoc());
  ASSERT_EQ(1, f.ord());  // "test"
  ASSERT_EQ(-1, f.nextDoc());
}

TEST_F(StrColTest, deleteAndMergeMultiValued) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_ss");
  
  // Add documents in first segment
  f.startIndexing();
  f.addStrings(5, {"hello", "world"});    // hello=ord 2, world=ord 6
  f.addStrings(7, {"test", "data", "hello", "string", "banana"});  // both unique terms and terms in other docs.
  f.addStrings(11, {"final", "string"});  // final=ord 3, string=ord 4
  testIndex.deleteDoc(7);  // delete the middle document
  testIndex.flush();
  
  // Add document in second segment
  f.startIndexing();
  f.addStrings(0, {"apple", "banana"});   // apple=ord 1, banana=ord 2 (before global merge)
  testIndex.flush();
  
  testIndex.iw->mergeSegments();
  
  // After merge, global ordinals should be:
  // "apple"=1, "banana"=2, "final"=3, "hello"=4, "string"=5, "world"=6
  // (deleted terms "data" and "test" should not appear)
  
  f.startReading();
  std::vector<int64_t> ords;
  
  ASSERT_EQ(5, f.nextDoc());
  f.ords(ords);
  ASSERT_EQ(ords, vec(4l, 6l));  // "hello"=4, "world"=6
  
  ASSERT_EQ(10, f.nextDoc());  // renumbered from 11
  f.ords(ords);
  ASSERT_EQ(ords, vec(3l, 5l));  // "final"=3, "string"=5
  
  ASSERT_EQ(11, f.nextDoc());  // renumbered from 0 in second segment
  f.ords(ords);
  ASSERT_EQ(ords, vec(1l, 2l));  // "apple"=1, "banana"=2
  
  ASSERT_EQ(-1, f.nextDoc());
}


TEST_F(StrColTest, strColReaderBasic) {
  // Test StrColReader for column-only string storage
  TestIndex testIndex;
  TestField f(testIndex, "description_sc");
  
  // Add documents with some missing values
  f.startIndexing();
  f.add(0, "First document description");
  // Doc 1 has no value
  f.add(2, "Second doc with text");
  f.add(3, "");  // Empty string
  // Doc 4 has no value
  f.add(5, "Final document with a longer description text");
  
  testIndex.flush();
  
  // Now read using StrColReader
  testIndex.initReader();
  auto& segment = testIndex.reader->segments()[0];
  auto& postingsReader = segment.postingsReader();
  FieldReader fieldReader(postingsReader);
  bool found = fieldReader.seek("description_sc");
  ASSERT_TRUE(found);
  
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);
  
  StrColReader strReader(postingsReader, segFieldInfo);
  ASSERT_EQ(4, strReader.docsWithValue());  // 4 docs have values (0, 2, 3, 5)
  
  // Iterate through docs with values
  StrColReader::Iterator iter(strReader);
  
  ASSERT_EQ(0, iter.advance(0));
  auto val = iter.value();
  
  ASSERT_EQ("First document description", val);
  
  ASSERT_EQ(2, iter.next());  // Skip doc 1 which has no value
  ASSERT_EQ("Second doc with text", iter.value());
  
  ASSERT_EQ(3, iter.next());
  ASSERT_EQ("", iter.value());  // Empty string
  
  ASSERT_EQ(5, iter.next());  // Skip doc 4
  ASSERT_EQ("Final document with a longer description text", iter.value());
  
  ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  
  // Test advance
  StrColReader::Iterator iter2(strReader);
  ASSERT_EQ(2, iter2.advance(2));
  ASSERT_EQ("Second doc with text", iter2.value());
  
  ASSERT_EQ(5, iter2.advance(4));  // Advance to 4, but doc 4 has no value, so we get doc 5
  ASSERT_EQ("Final document with a longer description text", iter2.value());
  
  // Test static getValues method
  std::vector<int32_t> docIds = {0, 1, 2, 3, 4, 5};
  std::vector<std::pair<int32_t, std::string>> results;
  
  StrColReader::getValues(MemPool::threadLocal(), postingsReader, segFieldInfo, docIds,
    [&results](size_t idx, int32_t docid, std::string_view value) {
      results.emplace_back(docid, std::string(value));
    });
  
  ASSERT_EQ(4, results.size());
  ASSERT_EQ(0, results[0].first);
  ASSERT_EQ("First document description", results[0].second);
  ASSERT_EQ(2, results[1].first);
  ASSERT_EQ("Second doc with text", results[1].second);
  ASSERT_EQ(3, results[2].first);
  ASSERT_EQ("", results[2].second);
  ASSERT_EQ(5, results[3].first);
  ASSERT_EQ("Final document with a longer description text", results[3].second);
}


TEST_F(StrColTest, fixedSizeOptimization) {
  // Test that fixed-size strings skip the mono column
  {
    // All strings same size (10 chars)
    TestIndex testIndex;
    TestField f(testIndex, "fixed_sc");

    f.startIndexing();
    f.add(0, "0123456789");
    f.add(1, "abcdefghij");
    f.add(3, "!@#$%^&*()");

    testIndex.flush();

    // Read and verify values
    testIndex.initReader();
    auto& segment = testIndex.reader->segments()[0];
    auto& postingsReader = segment.postingsReader();
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek("fixed_sc");
    ASSERT_TRUE(found);

    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);

    // Test reading with StrColReader
    StrColReader strReader(postingsReader, segFieldInfo);

    ASSERT_EQ(nullptr, strReader.getEndValueRankReader());  // No end rank reader for fixed-size strings

    StrColReader::Iterator iter(strReader);

    ASSERT_EQ(0, iter.advance(0));
    ASSERT_EQ("0123456789", iter.value());

    ASSERT_EQ(1, iter.next());
    ASSERT_EQ("abcdefghij", iter.value());

    ASSERT_EQ(3, iter.next());
    ASSERT_EQ("!@#$%^&*()", iter.value());

    ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  }
}

TEST_F(StrColTest, mergeNonIndexedStrCol) {
  // Test merging of non-indexed string columns (column-only storage)
  
  // Simple test first - single doc per segment
  {
    TestIndex testIndex;
    TestField f(testIndex, "simple_sc");
    
    // First segment
    f.startIndexing();
    f.add(0, "First");
    testIndex.flush();
    
    // Verify first segment alone works
    testIndex.initReader();
    auto& seg1 = testIndex.reader->segments()[0];
    auto& pr1 = seg1.postingsReader();
    FieldReader fr1(pr1);
    ASSERT_TRUE(fr1.seek("simple_sc"));
    SegFieldInfo sfi1;
    fr1.readFieldInfo(sfi1);
    // Check the field type and flags
    ASSERT_EQ(FieldType::Type::STRING, sfi1.type) << "Field type should be STRING";
    ASSERT_EQ(FieldType::COLUMN_STORED, sfi1.flags) << "Field flags should be COLUMN_STORED only";
    StrColReader sr1(pr1, sfi1);
    ASSERT_EQ(1, sr1.docsWithValue());
    
    // Second segment
    f.startIndexing();
    f.add(0, "Second");
    testIndex.flush();
    
    // Merge segments
    testIndex.iw->mergeSegments();
    
    // Verify merged data
    testIndex.initReader();
    ASSERT_EQ(1, testIndex.reader->segments().size());  // Should have 1 merged segment
    auto& segment = testIndex.reader->segments()[0];
    auto& postingsReader = segment.postingsReader();
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek("simple_sc");
    ASSERT_TRUE(found) << "Field simple_sc not found after merge";
    
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    
    StrColReader strReader(postingsReader, segFieldInfo);
    ASSERT_EQ(2, strReader.docsWithValue()) << "Expected 2 docs with values after merge";
    
    StrColReader::Iterator iter(strReader);
    
    ASSERT_EQ(0, iter.advance(0));
    ASSERT_EQ("First", iter.value());
    
    ASSERT_EQ(1, iter.next());
    ASSERT_EQ("Second", iter.value());
    
    ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  }
  
  // Basic dense merge
  {
    TestIndex testIndex;
    TestField f(testIndex, "content_sc");
    
    // First segment
    f.startIndexing();
    f.add(0, "First document");
    f.add(1, "Second document with longer text");
    testIndex.flush();
    
    // Second segment
    f.startIndexing();
    f.add(0, "Third doc");
    f.add(1, "Fourth");
    testIndex.flush();
    
    // Merge segments
    testIndex.iw->mergeSegments();
    
    // Verify merged data
    testIndex.initReader();
    auto& segment = testIndex.reader->segments()[0];
    auto& postingsReader = segment.postingsReader();
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek("content_sc");
    ASSERT_TRUE(found);
    
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    
    StrColReader strReader(postingsReader, segFieldInfo);
    ASSERT_EQ(4, strReader.docsWithValue());
    
    StrColReader::Iterator iter(strReader);
    
    ASSERT_EQ(0, iter.advance(0));
    ASSERT_EQ("First document", iter.value());
    
    ASSERT_EQ(1, iter.next());
    ASSERT_EQ("Second document with longer text", iter.value());
    
    ASSERT_EQ(2, iter.next());
    ASSERT_EQ("Third doc", iter.value());
    
    ASSERT_EQ(3, iter.next());
    ASSERT_EQ("Fourth", iter.value());
    
    ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  }
  
  // Sparse merge
  {
    TestIndex testIndex;
    TestField f(testIndex, "sparse_sc");
    
    // First segment - sparse
    f.startIndexing();
    f.add(10, "Document at 10");
    f.add(100, "Document at 100");
    testIndex.flush();
    
    // Second segment - sparse
    f.startIndexing();
    f.add(5, "Document at 5");
    f.add(50, "Document at 50");
    testIndex.flush();
    
    // Merge segments
    testIndex.iw->mergeSegments();
    
    // Verify merged data
    testIndex.initReader();
    auto& segment = testIndex.reader->segments()[0];
    auto& postingsReader = segment.postingsReader();
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek("sparse_sc");
    ASSERT_TRUE(found);
    
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    
    StrColReader strReader(postingsReader, segFieldInfo);
    ASSERT_EQ(4, strReader.docsWithValue());
    
    StrColReader::Iterator iter(strReader);
    
    ASSERT_EQ(10, iter.advance(0));
    ASSERT_EQ("Document at 10", iter.value());
    
    ASSERT_EQ(100, iter.next());
    ASSERT_EQ("Document at 100", iter.value());
    
    ASSERT_EQ(106, iter.next());  // 100 + 1 + 5
    ASSERT_EQ("Document at 5", iter.value());
    
    ASSERT_EQ(151, iter.next());  // 100 + 1 + 50
    ASSERT_EQ("Document at 50", iter.value());
    
    ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  }
  
  // Merge with deletions
  {
    TestIndex testIndex;
    TestField f(testIndex, "delete_sc");
    
    // First segment
    f.startIndexing();
    f.add(0, "Keep this");
    f.add(1, "Delete this");
    f.add(2, "Also keep this");
    testIndex.deleteDoc(1);  // Delete middle document
    testIndex.flush();
    
    // Second segment
    f.startIndexing();
    f.add(0, "New document");
    testIndex.flush();
    
    // Merge segments
    testIndex.iw->mergeSegments();
    
    // Verify merged data - deleted doc should not appear
    testIndex.initReader();
    auto& segment = testIndex.reader->segments()[0];
    auto& postingsReader = segment.postingsReader();
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek("delete_sc");
    ASSERT_TRUE(found);
    
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    
    StrColReader strReader(postingsReader, segFieldInfo);
    ASSERT_EQ(3, strReader.docsWithValue());  // Only 3 docs (deleted one excluded)
    
    StrColReader::Iterator iter(strReader);
    
    ASSERT_EQ(0, iter.advance(0));
    ASSERT_EQ("Keep this", iter.value());
    
    ASSERT_EQ(1, iter.next());  // Doc 2 becomes doc 1 after deletion
    ASSERT_EQ("Also keep this", iter.value());
    
    ASSERT_EQ(2, iter.next());  // New document from second segment
    ASSERT_EQ("New document", iter.value());
    
    ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  }
  
  // Fixed-size optimization preserved during merge
  {
    TestIndex testIndex;
    TestField f(testIndex, "fixed_merge_sc");
    
    // First segment - all 5 chars
    f.startIndexing();
    f.add(0, "12345");
    f.add(1, "abcde");
    testIndex.flush();
    
    // Second segment - all 5 chars
    f.startIndexing();
    f.add(0, "fghij");
    f.add(1, "67890");
    testIndex.flush();
    
    // Merge segments
    testIndex.iw->mergeSegments();
    
    // Verify fixed-size optimization is preserved
    testIndex.initReader();
    auto& segment = testIndex.reader->segments()[0];
    auto& postingsReader = segment.postingsReader();
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek("fixed_merge_sc");
    ASSERT_TRUE(found);
    
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    
    StrColReader strReader(postingsReader, segFieldInfo);
    ASSERT_EQ(nullptr, strReader.getEndValueRankReader());  // Should still have no end rank reader
    
    StrColReader::Iterator iter(strReader);
    
    ASSERT_EQ(0, iter.advance(0));
    ASSERT_EQ("12345", iter.value());
    
    ASSERT_EQ(1, iter.next());
    ASSERT_EQ("abcde", iter.value());
    
    ASSERT_EQ(2, iter.next());
    ASSERT_EQ("fghij", iter.value());
    
    ASSERT_EQ(3, iter.next());
    ASSERT_EQ("67890", iter.value());
    
    ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  }
}

TEST_F(StrColTest, BasicMultiValuedColumnStoredStrings) {
  CollectionHelper helper;
  
  // Add documents with multi-valued column-stored-only string fields
  {
    auto doc = flatdoc("id_s", "doc1");
    // Add multi-valued _ssc field
    doc.push_back({"tags_ssc", std::vector<std::string>{"programming", "c++", "search"}});
    helper.index(doc, UpdateMessage::NO_COMMIT);
  }
  
  {
    auto doc = flatdoc("id_s", "doc2");
    // Add different number of values
    doc.push_back({"tags_ssc", std::vector<std::string>{"java", "database"}});
    helper.index(doc, UpdateMessage::NO_COMMIT);
  }
  
  {
    auto doc = flatdoc("id_s", "doc3");
    // Single value in multi-valued field
    doc.push_back({"tags_ssc", std::vector<std::string>{"python"}});
    helper.index(doc, UpdateMessage::COMMIT);
  }
  
  // Search and retrieve the _ssc field
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q")
      .getNumber()
      .limit(10)
      .allQuery()
      // Request fields including the multi-valued column-stored field
      .fields({"id_s", "tags_ssc"});
  req->execute();
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(3, docs->found.value_or(0));

  // Verify the returned multi-valued fields
  const auto& columns = docs->columns;
  ASSERT_TRUE(columns.contains("tags_ssc"));

  const auto& tagsCol = columns.at("tags_ssc");
  ASSERT_TRUE(std::holds_alternative<luxir::api::ArrArrStr>(tagsCol.kind));
  const auto& multiTags = std::get<luxir::api::ArrArrStr>(tagsCol.kind);

  ASSERT_EQ(3, multiTags.v.size());

  // Find doc1 and verify its tags
  const auto& idCol = std::get<luxir::api::ColStr>(columns.at("id_s").kind);
  for (int i = 0; i < 3; i++) {
    if (idCol.v[i] == "doc1") {
      auto& tags = multiTags.v[i].v;
      ASSERT_EQ(3, tags.size());
      ASSERT_EQ("programming", tags[0]);
      ASSERT_EQ("c++", tags[1]);
      ASSERT_EQ("search", tags[2]);
    } else if (idCol.v[i] == "doc2") {
      auto& tags = multiTags.v[i].v;
      ASSERT_EQ(2, tags.size());
      ASSERT_EQ("java", tags[0]);
      ASSERT_EQ("database", tags[1]);
    } else if (idCol.v[i] == "doc3") {
      auto& tags = multiTags.v[i].v;
      ASSERT_EQ(1, tags.size());
      ASSERT_EQ("python", tags[0]);
    }
  }
}

TEST_F(StrColTest, EmptyAndMissingValues) {
  CollectionHelper helper;
  
  // Document with empty array
  {
    auto doc = flatdoc("id_s", "doc1");
    // Empty array for _ssc field
    doc.push_back({"tags_ssc", std::vector<std::string>{}});
    helper.index(doc, UpdateMessage::NO_COMMIT);
  }
  
  // Document with no _ssc field
  {
    auto doc = flatdoc("id_s", "doc2");
    // No tags_ssc field at all
    helper.index(doc, UpdateMessage::NO_COMMIT);
  }
  
  // Document with values
  {
    auto doc = flatdoc("id_s", "doc3");
    doc.push_back({"tags_ssc", std::vector<std::string>{"test"}});
    helper.index(doc, UpdateMessage::COMMIT);
  }
  
  // Search and retrieve
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q")
      .getNumber()
      .limit(10)
      .allQuery()
      .fields({"id_s", "tags_ssc"});
  req->execute();
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(3, docs->found.value_or(0));

  // Verify handling of empty and missing values
  const auto& columns = docs->columns;
  const auto& idCol = std::get<luxir::api::ColStr>(columns.at("id_s").kind);

  if (columns.contains("tags_ssc")) {
    const auto& tagsCol = columns.at("tags_ssc");
    if (std::holds_alternative<luxir::api::ArrArrStr>(tagsCol.kind)) {
      const auto& multiTags = std::get<luxir::api::ArrArrStr>(tagsCol.kind);

      for (int i = 0; i < 3; i++) {
        if (idCol.v[i] == "doc1") {
          // Empty array should still be present but with 0 values
          ASSERT_EQ(0, multiTags.v[i].v.size());
        } else if (idCol.v[i] == "doc2") {
          // Missing field - implementation dependent
          // Either missing or empty array
        } else if (idCol.v[i] == "doc3") {
          ASSERT_EQ(1, multiTags.v[i].v.size());
          ASSERT_EQ("test", multiTags.v[i].v[0]);
        }
      }
    }
  }
}


TEST_F(StrColTest, MultiValuedFixedSizeOptimization) {
  // Test that multi-valued fields with uniform block sizes use fixed-size optimization
  CollectionHelper helper;
  
  // Add documents with multi-valued fields where all values are the same size (5 chars).
  // This exercises the fixed-size path where no endOffsetReader is needed; the per-doc
  // endValueRankReader is still present because it's a multi-valued field.
  {
    auto doc = flatdoc("id_s", "doc1");
    doc.push_back({"uniform_ssc", std::vector<std::string>{"aaaaa", "bbbbb"}});
    helper.index(doc, UpdateMessage::NO_COMMIT);
  }
  
  {
    auto doc = flatdoc("id_s", "doc2");
    doc.push_back({"uniform_ssc", std::vector<std::string>{"ccccc", "ddddd"}});
    helper.index(doc, UpdateMessage::NO_COMMIT);
  }
  
  {
    auto doc = flatdoc("id_s", "doc3");
    doc.push_back({"uniform_ssc", std::vector<std::string>{"eeeee", "fffff"}});
    helper.index(doc, UpdateMessage::COMMIT);
  }
  
  // Verify the field was written with fixed-size optimization
  auto indexWriter = helper.getIndexWriter();
  auto reader = indexWriter->getIndexReader();
  ASSERT_TRUE(reader != nullptr);
  ASSERT_GT(reader->segments().size(), 0);
  
  auto& segment = reader->segments()[0];
  auto& postingsReader = segment.postingsReader();
  FieldReader fieldReader(postingsReader);
  bool found = fieldReader.seek("uniform_ssc");
  ASSERT_TRUE(found);
  
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);
  
  // Fixed-size multi-valued: endOffsetReader (mono2Loc) is absent, endValueRankReader (monoLoc) is present.
  ASSERT_EQ(0, segFieldInfo.mono2Loc.offset());
  ASSERT_EQ(0, segFieldInfo.mono2Loc.filenum());
  ASSERT_EQ(5, segFieldInfo.mono2MetaOff);  // fixed value size
  ASSERT_NE(0, segFieldInfo.monoLoc.offset());  // endValueRankReader present for multi-valued
  
  // Verify we can still read the values correctly
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q")
      .getNumber()
      .limit(10)
      .allQuery()
      .fields({"id_s", "uniform_ssc"});
  req->execute();
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(3, docs->found.value_or(0));

  // Verify the values are correct
  const auto& columns = docs->columns;
  ASSERT_TRUE(columns.contains("uniform_ssc"));
  const auto& uniformCol = columns.at("uniform_ssc");
  ASSERT_TRUE(std::holds_alternative<luxir::api::ArrArrStr>(uniformCol.kind));
  const auto& multiUniform = std::get<luxir::api::ArrArrStr>(uniformCol.kind);

  ASSERT_EQ(3, multiUniform.v.size());

  // Check each document has the expected values
  for (int i = 0; i < 3; i++) {
    auto& vals = multiUniform.v[i].v;
    ASSERT_EQ(2, vals.size());
    ASSERT_EQ(5, vals[0].size());
    ASSERT_EQ(5, vals[1].size());
  }
}

TEST_F(StrColTest, DocValuesManyValuesPerDoc) {
  // Exercise StrColReader::DocValues on both the variable-size and fixed-size paths
  // with enough values per doc to cross MonoReader::BulkValues' 128-value sub-block
  // boundary, catching regressions in block cache management.
  constexpr int N = 300;

  CollectionHelper helper;

  // Variable-size: every value has a different length (1..N).
  std::vector<std::string> varValues;
  varValues.reserve(N);
  for (int i = 0; i < N; i++) {
    varValues.emplace_back((size_t)(i + 1), (char)('a' + (i % 26)));
  }

  // Fixed-size: all values same size, different content.
  std::vector<std::string> fixValues;
  fixValues.reserve(N);
  for (int i = 0; i < N; i++) {
    std::string v(8, '\0');
    for (int j = 0; j < 8; j++) v[j] = (char)('a' + ((i + j) % 26));
    fixValues.emplace_back(std::move(v));
  }

  {
    auto doc = flatdoc("id_s", "doc1");
    doc.push_back({"chunks_ssc", varValues});
    doc.push_back({"uniform_ssc", fixValues});
    helper.index(doc, UpdateMessage::COMMIT);
  }

  auto indexWriter = helper.getIndexWriter();
  auto reader = indexWriter->getIndexReader();
  ASSERT_TRUE(reader != nullptr);
  ASSERT_GT(reader->segments().size(), 0);
  auto& segment = reader->segments()[0];
  auto& postingsReader = segment.postingsReader();

  // Variable-size path: endOffsetReader present; BulkValues wraps MonoReader::BulkValues.
  {
    FieldReader fr(postingsReader);
    ASSERT_TRUE(fr.seek("chunks_ssc"));
    SegFieldInfo sfi;
    fr.readFieldInfo(sfi);
    ASSERT_NE(0, sfi.mono2Loc.offset()) << "expected endOffsetReader for variable-size";

    StrColReader strReader(postingsReader, sfi);
    ASSERT_EQ((int64_t)N, strReader.numValues());
    ASSERT_TRUE(strReader.isMultiValued());
    ASSERT_FALSE(strReader.isFixedSize());

    StrColReader::DocValues docValues(strReader);
    for (int i = 0; i < N; i++) {
      ASSERT_EQ(varValues[i], docValues.valueAt(i)) << "variable-size valueAt mismatch at " << i;
    }
    // Non-sequential access: skipping forward, jumping back, to exercise cache-miss paths.
    ASSERT_EQ(varValues[250], docValues.valueAt(250));
    ASSERT_EQ(varValues[50],  docValues.valueAt(50));
    ASSERT_EQ(varValues[251], docValues.valueAt(251));
  }

  // Fixed-size path: no endOffsetReader; BulkValues uses pointer arithmetic.
  {
    FieldReader fr(postingsReader);
    ASSERT_TRUE(fr.seek("uniform_ssc"));
    SegFieldInfo sfi;
    fr.readFieldInfo(sfi);
    ASSERT_EQ(0, sfi.mono2Loc.offset()) << "expected no endOffsetReader for fixed-size";
    ASSERT_EQ(8, sfi.mono2MetaOff);

    StrColReader strReader(postingsReader, sfi);
    ASSERT_TRUE(strReader.isFixedSize());
    ASSERT_EQ(8, strReader.fixedValueSize());

    StrColReader::DocValues docValues(strReader);
    for (int i = 0; i < N; i++) {
      ASSERT_EQ(fixValues[i], docValues.valueAt(i)) << "fixed-size valueAt mismatch at " << i;
    }
  }
}

