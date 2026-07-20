#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "solux/search/OrdMap.h"
#include "solux/search/IndexReader.h"
#include "solux/reader/IntColReader.h"

using namespace solux;
using namespace solux::test;

namespace {

class OrdMapEncodingGuard {
  OrdMap::DeltaEncoding saved;

public:
  explicit OrdMapEncodingGuard(OrdMap::DeltaEncoding encoding)
      : saved(OrdMap::setDeltaEncodingForTests(encoding)) {}
  ~OrdMapEncodingGuard() { OrdMap::setDeltaEncodingForTests(saved); }
};

} // namespace

class OrdMapTest : public ::testing::Test {
protected:
  void SetUp() override {
    helper = std::make_unique<CollectionHelper>();
  }

  void TearDown() override {
    helper.reset();
  }

  std::unique_ptr<CollectionHelper> helper;
};


// Basic Functionality - Empty Index
TEST_F(OrdMapTest, EmptyIndex) {
  // Don't index any documents
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();

  EXPECT_EQ(reader->ordMaps->dataMap.size(), 0);

  auto ordMap = reader->getOrdMap("field1_s");

  // OrdMap should be null for empty index
  EXPECT_EQ(ordMap, nullptr);
  // Make sure the null wasn't cached.
  EXPECT_EQ(reader->ordMaps->dataMap.size(), 0);

  // If field doesn't exist, same thing.
  helper->index({{"otherfield_s", "apple"}, {"id", "1"}}, UpdateMessage::COMMIT);
  helper->index({{"otherfield_s", "banana"}, {"id", "2"}}, UpdateMessage::COMMIT);
  ordMap = reader->getOrdMap("field1_s");
  EXPECT_EQ(ordMap, nullptr);
  // Make sure the null wasn't cached.
  EXPECT_EQ(reader->ordMaps->dataMap.size(), 0);
}

// Single Segment with Simple Terms
TEST_F(OrdMapTest, SingleSegmentSimpleTerms) {
  // Index documents with different terms in field1
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->index({{"field1_s", "cherry"}, {"id", "3"}});
  helper->index({{"field1_s", "apple"}, {"id", "4"}}); // duplicate term
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  
  // Debug: Check if we have segments and the field exists
  const auto& segments = reader->segments();
  std::cout << "Number of segments: " << segments.size() << std::endl;
  EXPECT_EQ(segments.size(), 1) << "Expected exactly one segment";

  auto ordMap = reader->getOrdMap("field1_s");

  // With only one segment, OrdMap now returns a valid instance for identity mapping
  EXPECT_NE(ordMap, nullptr) << "OrdMap should be valid for single segment";
  
  // Check that it has the correct properties for single segment
  EXPECT_EQ(ordMap->firstFullSeg(), 0) << "First full segment should be 0";
  EXPECT_EQ(ordMap->numOrds(), 3) << "Should have 3 unique terms (apple, banana, cherry)";
}

// Multiple Segments with Disjoint Terms
TEST_F(OrdMapTest, MultipleSegmentsDisjointTerms) {
  // First segment
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->commit();
  
  // Second segment with different terms
  helper->index({{"field1_s", "cherry"}, {"id", "3"}});
  helper->index({{"field1_s", "date"}, {"id", "4"}});
  helper->commit();
  
  // Third segment with yet different terms
  helper->index({{"field1_s", "elderberry"}, {"id", "5"}});
  helper->index({{"field1_s", "fig"}, {"id", "6"}});
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");

  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 6); // 6 unique terms total
  
  // Check segment mappings
  auto seg0Mapping = ordMap->getSegToGlobal(0);
  EXPECT_EQ(seg0Mapping.numOrds, 2); // apple, banana
  EXPECT_EQ(seg0Mapping.bits, 0); // global-prefix identity
  
  auto seg1Mapping = ordMap->getSegToGlobal(1);
  EXPECT_EQ(seg1Mapping.numOrds, 2); // cherry, date
  EXPECT_GT(seg1Mapping.bits, 0); // needs mapping
  
  auto seg2Mapping = ordMap->getSegToGlobal(2);
  EXPECT_EQ(seg2Mapping.numOrds, 2); // elderberry, fig
  EXPECT_GT(seg2Mapping.bits, 0); // needs mapping
  
  // Should have firstSegs and globDeltas
  EXPECT_NE(ordMap->getFirstSegs(), nullptr);
  EXPECT_NE(ordMap->getGlobDeltas(), nullptr);

  auto ordMap2 = reader->getOrdMap("field1_s");
  EXPECT_EQ(ordMap.get(), ordMap2.get()); // should be cached

}

// Multiple Segments with Overlapping Terms
TEST_F(OrdMapTest, MultipleSegmentsOverlappingTerms) {
  // First segment
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->index({{"field1_s", "cherry"}, {"id", "3"}});
  helper->commit();
  
  // Second segment with some overlapping terms
  helper->index({{"field1_s", "banana"}, {"id", "4"}}); // overlap
  helper->index({{"field1_s", "date"}, {"id", "5"}}); // new
  helper->index({{"field1_s", "cherry"}, {"id", "6"}}); // overlap
  helper->commit();
  
  // Third segment with more overlaps
  helper->index({{"field1_s", "apple"}, {"id", "7"}}); // overlap
  helper->index({{"field1_s", "elderberry"}, {"id", "8"}}); // new
  helper->index({{"field1_s", "date"}, {"id", "9"}}); // overlap
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  
  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 5); // 5 unique terms: apple, banana, cherry, date, elderberry
  
  // The first segment is a global prefix; the others need packed mappings.
  for (int i = 0; i < 3; i++) {
    auto segMapping = ordMap->getSegToGlobal(i);
    EXPECT_GT(segMapping.numOrds, 0);
    EXPECT_EQ(segMapping.bits == 0, i == 0);
  }
  
  // Should have firstSegs and globDeltas
  EXPECT_NE(ordMap->getFirstSegs(), nullptr);
  EXPECT_NE(ordMap->getGlobDeltas(), nullptr);
}

// Field in Some Segments
TEST_F(OrdMapTest, FieldInSomeSegments) {
  // First segment with field1
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->commit();
  
  // Second segment without field1
  helper->index({{"other_field_s", "value"}, {"id", "3"}});
  helper->index({{"other_field_s", "value2"}, {"id", "4"}});
  helper->commit();
  
  // Third segment with field1
  helper->index({{"field1_s", "cherry"}, {"id", "5"}});
  helper->index({{"field1_s", "date"}, {"id", "6"}});
  helper->commit();
  
  // Fourth segment without field1
  helper->index({{"different_field_s", "value3"}, {"id", "7"}});
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  
  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 4); // apple, banana, cherry, date
  
  // Check mappings - segments without field should have numOrds=0 and null mapping
  auto seg0 = ordMap->getSegToGlobal(0);
  EXPECT_EQ(seg0.numOrds, 2); // has apple, banana
  EXPECT_EQ(seg0.bits, 0); // global-prefix identity
  
  auto seg1 = ordMap->getSegToGlobal(1);
  EXPECT_EQ(seg1.numOrds, 0); // no field1
  EXPECT_EQ(seg1.deltas, nullptr);
  
  auto seg2 = ordMap->getSegToGlobal(2);
  EXPECT_EQ(seg2.numOrds, 2); // has cherry, date
  EXPECT_GT(seg2.bits, 0);
  
  auto seg3 = ordMap->getSegToGlobal(3);
  EXPECT_EQ(seg3.numOrds, 0); // no field1
  EXPECT_EQ(seg3.deltas, nullptr);
}

// Empty Field in Segment
TEST_F(OrdMapTest, EmptyFieldInSegment) {
  // First segment with normal field values
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->commit();
  
  // Second segment - documents exist but field1 is empty string or missing
  helper->index({{"field1_s", ""}, {"id", "3"}});  // empty string
  helper->index({{"other_field_s", "value"}, {"id", "4"}});  // field1 missing
  helper->commit();
  
  // Third segment with normal values
  helper->index({{"field1_s", "cherry"}, {"id", "5"}});
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  
  ASSERT_NE(ordMap, nullptr);
  // Empty string is still a term, so we should have 4 terms: "", "apple", "banana", "cherry"
  EXPECT_EQ(ordMap->numOrds(), 4);
}

// Test 8: Edge Case - Segment With All Terms
TEST_F(OrdMapTest, SegmentWithAllTerms) {
  // First small segment
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->commit();
  
  // Second small segment
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->commit();
  
  // Third segment with all terms from previous segments plus more
  helper->index({{"field1_s", "apple"}, {"id", "3"}});
  helper->index({{"field1_s", "banana"}, {"id", "4"}});
  helper->index({{"field1_s", "cherry"}, {"id", "5"}});
  helper->index({{"field1_s", "date"}, {"id", "6"}});
  helper->commit();
  
  // Fourth segment with subset of terms
  helper->index({{"field1_s", "banana"}, {"id", "7"}});
  helper->index({{"field1_s", "cherry"}, {"id", "8"}});
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  
  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 4); // apple, banana, cherry, date
  
  // Check which segment has all terms
  EXPECT_EQ(ordMap->firstFullSeg(), 2); // third segment (index 2) has all terms
  
  // Segments with all terms should have null mapping
  auto seg2 = ordMap->getSegToGlobal(2);
  EXPECT_EQ(seg2.numOrds, 4); // has all 4 terms
  EXPECT_EQ(seg2.deltas, nullptr); // null because it has all terms
  
  // When a segment has all terms, we don't need firstSegs/globDeltas
  EXPECT_EQ(ordMap->getFirstSegs(), nullptr);
  EXPECT_EQ(ordMap->getGlobDeltas(), nullptr);
  
  // Other segments should still have mappings
  auto seg0 = ordMap->getSegToGlobal(0);
  EXPECT_EQ(seg0.numOrds, 1); // only apple
  EXPECT_EQ(seg0.bits, 0); // global-prefix identity
  
  auto seg1 = ordMap->getSegToGlobal(1);
  EXPECT_EQ(seg1.numOrds, 1); // only banana
  EXPECT_GT(seg1.bits, 0);
  
  auto seg3 = ordMap->getSegToGlobal(3);
  EXPECT_EQ(seg3.numOrds, 2); // banana, cherry
  EXPECT_GT(seg3.bits, 0);
}

// Verify Correct Mappings
TEST_F(OrdMapTest, SegToGlobalMapping) {
  // Create segments with known terms to verify mapping
  // Segment 0: banana, date
  helper->index({{"field1_s", "banana"}, {"id", "1"}});
  helper->index({{"field1_s", "date"}, {"id", "2"}});
  helper->commit();
  
  // Segment 1: apple, cherry, elderberry  
  helper->index({{"field1_s", "apple"}, {"id", "3"}});
  helper->index({{"field1_s", "cherry"}, {"id", "4"}});
  helper->index({{"field1_s", "elderberry"}, {"id", "5"}});
  helper->commit();
  
  // Segment 2: banana, cherry
  helper->index({{"field1_s", "banana"}, {"id", "6"}});
  helper->index({{"field1_s", "cherry"}, {"id", "7"}});
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  
  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 5); // apple, banana, cherry, date, elderberry (sorted)
  
  // Verify segment 0 mapping
  auto seg0 = ordMap->getSegToGlobal(0);
  EXPECT_EQ(seg0.numOrds, 2);
  ASSERT_GT(seg0.bits, 0);
  
  // In segment 0: banana=0, date=1
  // In global: apple=0, banana=1, cherry=2, date=3, elderberry=4
  // So mapping should be: 0->1, 1->2
  EXPECT_EQ(seg0.deltaAt(0), 1); // banana: seg ord 0 -> global ord 1
  EXPECT_EQ(seg0.deltaAt(1), 2); // date: seg ord 1 -> global ord 2 (actual correct value)
  
  // Verify segment 1 mapping  
  auto seg1 = ordMap->getSegToGlobal(1);
  EXPECT_EQ(seg1.numOrds, 3);
  ASSERT_GT(seg1.bits, 0);
  
  // In segment 1: apple=0, cherry=1, elderberry=2
  // Mapping should be: 0->0, 1->1, 2->2
  EXPECT_EQ(seg1.deltaAt(0), 0); // apple: seg ord 0 -> global ord 0
  EXPECT_EQ(seg1.deltaAt(1), 1); // cherry: seg ord 1 -> global ord 1 (actual correct value)
  EXPECT_EQ(seg1.deltaAt(2), 2); // elderberry: seg ord 2 -> global ord 2 (actual correct value)
  
  // Verify segment 2 mapping
  auto seg2 = ordMap->getSegToGlobal(2);
  EXPECT_EQ(seg2.numOrds, 2);
  ASSERT_GT(seg2.bits, 0);
  
  // In segment 2: banana=0, cherry=1
  // Mapping should be: 0->1, 1->1 (updated to match actual correct behavior)
  EXPECT_EQ(seg2.deltaAt(0), 1); // banana: seg ord 0 -> global ord 1
  EXPECT_EQ(seg2.deltaAt(1), 1); // cherry: seg ord 1 -> global ord 1 (actual correct value)
}

// Segment-to-Global Mapping - Null Cases
TEST_F(OrdMapTest, SegToGlobalNullCases) {
  // Segment 0: has all terms that will exist
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->commit();
  
  // Segment 1: no field1
  helper->index({{"other_field_s", "value"}, {"id", "3"}});
  helper->commit();
  
  // Segment 2: subset of terms
  helper->index({{"field1_s", "apple"}, {"id", "4"}});
  helper->commit();
  
  // Segment 3: has all terms again
  helper->index({{"field1_s", "apple"}, {"id", "5"}});
  helper->index({{"field1_s", "banana"}, {"id", "6"}});
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  
  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 2); // apple, banana
  
  // Segment 0 has all terms - should be null
  auto seg0 = ordMap->getSegToGlobal(0);
  EXPECT_EQ(seg0.numOrds, 2);
  EXPECT_EQ(seg0.deltas, nullptr); // null for segment with all terms
  
  // Segment 1 has no field - should be null
  auto seg1 = ordMap->getSegToGlobal(1);
  EXPECT_EQ(seg1.numOrds, 0);
  EXPECT_EQ(seg1.deltas, nullptr); // null for segment without field
  
  // Segment 2 is a global-prefix subset, so its mapping is identity.
  auto seg2 = ordMap->getSegToGlobal(2);
  EXPECT_EQ(seg2.numOrds, 1);
  EXPECT_EQ(seg2.bits, 0);
  
  // Segment 3 has all terms - should be null
  auto seg3 = ordMap->getSegToGlobal(3);
  EXPECT_EQ(seg3.numOrds, 2);
  EXPECT_EQ(seg3.deltas, nullptr); // null for segment with all terms

  // check that the global mappings are absent
  EXPECT_EQ(ordMap->getFirstSegs(), nullptr);
  EXPECT_EQ(ordMap->getGlobDeltas(), nullptr);
}

// Global-to-Segment Reverse Mapping
TEST_F(OrdMapTest, GlobalToSegmentReverseMapping) {
  // Create multiple segments with overlapping terms to ensure we have 
  // non-trivial firstSegs/globDeltas columns
  
  // Segment 0: banana, date
  helper->index({{"field1_s", "banana"}, {"id", "1"}});
  helper->index({{"field1_s", "date"}, {"id", "2"}});
  helper->commit();
  
  // Segment 1: apple, cherry, elderberry
  helper->index({{"field1_s", "apple"}, {"id", "3"}});
  helper->index({{"field1_s", "cherry"}, {"id", "4"}});
  helper->index({{"field1_s", "elderberry"}, {"id", "5"}});
  helper->commit();
  
  // Segment 2: banana, cherry, fig
  helper->index({{"field1_s", "banana"}, {"id", "6"}});
  helper->index({{"field1_s", "cherry"}, {"id", "7"}});
  helper->index({{"field1_s", "fig"}, {"id", "8"}});
  helper->commit();
  
  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  
  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 6); // apple, banana, cherry, date, elderberry, fig
  
  // Check if we have global columns (they should exist since no segment has all terms)
  if (ordMap->getFirstSegs() == nullptr || ordMap->getGlobDeltas() == nullptr) {
    // If no global columns, it means some segment has all terms - this is also valid
    // but then we can't test reverse mapping via global columns
    EXPECT_NE(ordMap->firstFullSeg(), -1) << "Expected at least one segment to have all terms";
    return;
  }
  
  // Expected global order: apple=0, banana=1, cherry=2, date=3, elderberry=4, fig=5
  // Create dense value accessors for the global columns
  IntColReader::SparseValues firstSegsValues(*ordMap->getFirstSegs());
  IntColReader::SparseValues globDeltasValues(*ordMap->getGlobDeltas());

  // Test reverse mapping for each global ordinal
  for (int globalOrd = 0; globalOrd < ordMap->numOrds(); globalOrd++) {
    // Get the first segment that contains this global ordinal
    int64_t segmentIdx = firstSegsValues.valueAt(globalOrd);
    int64_t delta = globDeltasValues.valueAt(globalOrd);
    
    // Calculate the segment ordinal
    int64_t segmentOrd = globalOrd - delta;
    
    // Verify the segment ordinal is valid for this segment
    auto segMapping = ordMap->getSegToGlobal(static_cast<int>(segmentIdx));

    auto actualGlobalOrd = segMapping.globalOrd(segmentOrd);
    EXPECT_EQ(actualGlobalOrd, globalOrd)
      << "Forward/reverse mapping inconsistency detected! "
      << "GlobalOrd " << globalOrd << " maps to segment " << segmentIdx
      << " at segmentOrd " << segmentOrd << " (via reverse mapping), "
      << "but forward mapping says segmentOrd " << segmentOrd
      << " maps to globalOrd " << actualGlobalOrd;
  }
}

TEST_F(OrdMapTest, GlobalPrefixSubsetUsesIdentityMapping) {
  helper->index({{"field1_s", "apple"}, {"id", "1"}});
  helper->index({{"field1_s", "banana"}, {"id", "2"}});
  helper->commit();
  helper->index({{"field1_s", "cherry"}, {"id", "3"}});
  helper->commit();

  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  ASSERT_NE(ordMap, nullptr);
  auto mapping = ordMap->getSegToGlobal(0);
  EXPECT_EQ(mapping.numOrds, 2);
  EXPECT_EQ(mapping.bits, 0);
  EXPECT_EQ(mapping.deltas, nullptr);
  EXPECT_EQ(mapping.globalOrd(0), 0);
  EXPECT_EQ(mapping.globalOrd(1), 1);
}

TEST_F(OrdMapTest, FlatDeltaFramesIncludeSafeTail) {
  for (int i = 0; i < 300; i += 2) {
    helper->index({{"field1_s", std::format("term{:04}", i)},
                   {"id", std::to_string(i)}});
  }
  helper->commit();
  for (int i = 1; i < 300; i += 2) {
    helper->index({{"field1_s", std::format("term{:04}", i)},
                   {"id", std::to_string(i)}});
  }
  helper->commit();

  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  ASSERT_NE(ordMap, nullptr);
  EXPECT_EQ(ordMap->numOrds(), 300);

  for (int seg = 0; seg < 2; seg++) {
    auto mapping = ordMap->getSegToGlobal(seg);
    ASSERT_EQ(mapping.numOrds, 150);
    ASSERT_GT(mapping.bits, 0);
    uint64_t deltas[128];
    mapping.unpackDeltas(0, 128, deltas);
    for (int i = 0; i < 128; i++) {
      EXPECT_EQ(mapping.globalOrd(i), i * 2 + seg);
      EXPECT_EQ(deltas[i], mapping.deltaAt(i));
    }
    mapping.unpackDeltas(128, 22, deltas);
    for (int i = 128; i < 150; i++) {
      EXPECT_EQ(mapping.globalOrd(i), i * 2 + seg);
      EXPECT_EQ(deltas[i - 128], mapping.deltaAt(i));
    }
    EXPECT_EQ(mapping.globalOrd(149), 298 + seg);
  }
}

TEST_F(OrdMapTest, PackedDeltaRunsCrossWidthBoundaries) {
  constexpr std::array<int, 7> positions = {0, 2, 5, 10, 19, 36, 69};
  for (int pos : positions) {
    helper->index({{"field1_s", std::format("term{:03}", pos)},
                   {"id", std::to_string(pos)}});
  }
  helper->commit();

  for (int pos = 0; pos < 70; pos++) {
    helper->index({{"field1_s", std::format("term{:03}", pos)},
                   {"id", std::format("all{}", pos)}});
  }
  helper->commit();

  auto reader = helper->getIndexWriter()->getIndexReader();
  auto ordMap = reader->getOrdMap("field1_s");
  ASSERT_NE(ordMap, nullptr);
  ASSERT_EQ(ordMap->numOrds(), 70);

  auto mapping = ordMap->getSegToGlobal(0);
  ASSERT_EQ(mapping.numOrds, (int64_t)positions.size());
  ASSERT_EQ(mapping.bits, 6);
  int64_t previousDelta = -1;
  for (int64_t localOrd = 0; localOrd < (int64_t)positions.size(); localOrd++) {
    int64_t delta = positions[localOrd] - localOrd;
    EXPECT_GE(delta, previousDelta);
    EXPECT_EQ(mapping.deltaAt(localOrd), delta);
    EXPECT_EQ(mapping.globalOrd(localOrd), positions[localOrd]);
    previousDelta = delta;
  }
}

TEST_F(OrdMapTest, FlatPredictedAndFitParityAcrossBlockBoundary) {
  constexpr int nTermsPerSegment = 4224;
  std::vector<Doc> evenDocs;
  std::vector<Doc> oddDocs;
  evenDocs.reserve(nTermsPerSegment);
  oddDocs.reserve(nTermsPerSegment);
  for (int localOrd = 0; localOrd < nTermsPerSegment; localOrd++) {
    int flip = localOrd % 257 == 128 ? 1 : 0;
    int even = localOrd * 2 + flip;
    int odd = localOrd * 2 + 1 - flip;
    std::string evenTerm = std::format("term{:05}", even);
    std::string oddTerm = std::format("term{:05}", odd);
    evenDocs.push_back({{"id", std::format("e{}", even)},
                        {"flat_s", evenTerm}, {"predicted_s", evenTerm},
                        {"fit_s", evenTerm}});
    oddDocs.push_back({{"id", std::format("o{}", odd)},
                       {"flat_s", oddTerm}, {"predicted_s", oddTerm},
                       {"fit_s", oddTerm}});
  }
  ASSERT_TRUE(helper->indexAll(evenDocs, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->indexAll(oddDocs, UpdateMessage::COMMIT).success);

  auto reader = helper->getIndexWriter()->getIndexReader();
  std::shared_ptr<OrdMap> flat;
  std::shared_ptr<OrdMap> predicted;
  std::shared_ptr<OrdMap> fit;
  {
    OrdMapEncodingGuard guard(OrdMap::DeltaEncoding::FLAT);
    flat = reader->getOrdMap("flat_s");
  }
  {
    OrdMapEncodingGuard guard(OrdMap::DeltaEncoding::PREDICTED);
    predicted = reader->getOrdMap("predicted_s");
  }
  {
    OrdMapEncodingGuard guard(OrdMap::DeltaEncoding::SINGLE_FIT);
    fit = reader->getOrdMap("fit_s");
  }
  ASSERT_NE(flat, nullptr);
  ASSERT_NE(predicted, nullptr);
  ASSERT_NE(fit, nullptr);
  ASSERT_EQ(flat->numOrds(), nTermsPerSegment * 2);
  ASSERT_EQ(predicted->numOrds(), flat->numOrds());
  ASSERT_EQ(fit->numOrds(), flat->numOrds());
  EXPECT_LT(predicted->sizeInBytes(), flat->sizeInBytes());
  EXPECT_LT(fit->sizeInBytes(), predicted->sizeInBytes());

  for (int seg = 0; seg < 2; seg++) {
    auto flatMapping = flat->getSegToGlobal(seg);
    auto predictedMapping = predicted->getSegToGlobal(seg);
    auto fitMapping = fit->getSegToGlobal(seg);
    ASSERT_EQ(flatMapping.numOrds, nTermsPerSegment);
    ASSERT_EQ(predictedMapping.numOrds, flatMapping.numOrds);
    ASSERT_EQ(fitMapping.numOrds, flatMapping.numOrds);
    ASSERT_FALSE(flatMapping.predicted());
    ASSERT_TRUE(predictedMapping.predicted());
    ASSERT_TRUE(fitMapping.singleFit());
    EXPECT_EQ(predictedMapping.residualBits, 1);
    EXPECT_EQ(fitMapping.residualBits, 1);
    EXPECT_NE(fitMapping.deltas, nullptr);
    EXPECT_EQ(fitMapping.blockMeta, nullptr);

    for (int64_t localOrd = 0; localOrd < nTermsPerSegment; localOrd++) {
      EXPECT_EQ(predictedMapping.deltaAt(localOrd),
                flatMapping.deltaAt(localOrd));
      EXPECT_EQ(fitMapping.deltaAt(localOrd), flatMapping.deltaAt(localOrd));
      EXPECT_EQ(predictedMapping.globalOrd(localOrd),
                flatMapping.globalOrd(localOrd));
      EXPECT_EQ(fitMapping.globalOrd(localOrd),
                flatMapping.globalOrd(localOrd));
    }

    for (uint64_t start : {3968u, 4096u}) {
      uint64_t flatDeltas[128];
      uint64_t predictedDeltas[128];
      uint64_t fitDeltas[128];
      flatMapping.unpackDeltas(start, 128, flatDeltas);
      predictedMapping.unpackDeltas(start, 128, predictedDeltas);
      fitMapping.unpackDeltas(start, 128, fitDeltas);
      for (uint32_t i = 0; i < 128; i++) {
        EXPECT_EQ(predictedDeltas[i], flatDeltas[i]);
        EXPECT_EQ(fitDeltas[i], flatDeltas[i]);
      }
    }
  }
}

TEST_F(OrdMapTest, PredictedAndFitResidualsUseSelect64) {
  constexpr uint32_t count = 128;
  std::array<uint64_t, count> deltas;
  constexpr uint64_t base = 1ull << 40;
  constexpr uint64_t step = 1ull << 35;
  for (uint32_t i = 0; i < count; i++) {
    deltas[i] = base + i + (i >= count / 2 ? step : 0);
  }

  auto plan = OrdColumnFormat::planBlock(
      std::span<const uint64_t>(deltas.data(), deltas.size()));
  ASSERT_GT(plan.info.bits, 32);
  plan.info.payloadOffset = 0;
  std::vector<char> payload;
  LinearPack::Writer writer(payload, plan.info.bits);
  uint64_t maxResidual = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint64_t residual = OrdColumnFormat::residual(plan.info, i, deltas[i]);
    maxResidual = std::max(maxResidual, residual);
    writer.append(residual);
  }
  writer.finish();
  ASSERT_GT(maxResidual, UINT32_MAX);

  OrdMap::SegToGlobal mapping;
  mapping.numOrds = count;
  mapping.deltas = payload.data();
  mapping.blockMeta = reinterpret_cast<const char*>(&plan.info);
  mapping.bits = (uint8_t)std::bit_width(deltas.back());
  mapping.residualBits = plan.info.bits;
  mapping.encoding = OrdMap::DeltaEncoding::PREDICTED;

  for (uint32_t i = 0; i < count; i++) {
    EXPECT_EQ(mapping.deltaAt(i), (int64_t)deltas[i]);
    EXPECT_EQ(mapping.globalOrd(i), (int64_t)(i + deltas[i]));
  }
  uint64_t decoded[count];
  mapping.unpackDeltas(0, count, decoded);
  for (uint32_t i = 0; i < count; i++) {
    EXPECT_EQ(decoded[i], deltas[i]);
  }

  auto fit = OrdColumnFormat::planLinearFit(
      std::span<const uint64_t>(deltas.data(), deltas.size()),
      OrdColumnFormat::SINGLE_FIT_SLOPE_SHIFT);
  ASSERT_TRUE(fit);
  ASSERT_GT(fit->bits, 32);
  std::vector<char> fitPayload;
  LinearPack::Writer fitWriter(fitPayload, fit->bits);
  maxResidual = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint64_t residual = OrdColumnFormat::residual(
        *fit, i, deltas[i], OrdColumnFormat::SINGLE_FIT_SLOPE_SHIFT);
    maxResidual = std::max(maxResidual, residual);
    fitWriter.append(residual);
  }
  fitWriter.finish();
  ASSERT_GT(maxResidual, UINT32_MAX);

  OrdMap::SegToGlobal fitMapping;
  fitMapping.numOrds = count;
  fitMapping.deltas = fitPayload.data();
  fitMapping.intercept = fit->intercept;
  fitMapping.scaledSlope = fit->scaledSlope;
  fitMapping.mask = LinearPack::mask64(fit->bits);
  fitMapping.bits = (uint8_t)std::bit_width(deltas.back());
  fitMapping.residualBits = fit->bits;
  fitMapping.encoding = OrdMap::DeltaEncoding::SINGLE_FIT;

  for (uint32_t i = 0; i < count; i++) {
    EXPECT_EQ(fitMapping.deltaAt(i), (int64_t)deltas[i]);
    EXPECT_EQ(fitMapping.globalOrd(i), (int64_t)(i + deltas[i]));
  }
  fitMapping.unpackDeltas(0, count, decoded);
  for (uint32_t i = 0; i < count; i++) {
    EXPECT_EQ(decoded[i], deltas[i]);
  }
}

TEST(OrdColumnFormatTest, SingleFitShiftAvoidsSegmentScaleDrift) {
  constexpr uint64_t count = 524289;
  std::vector<uint64_t> deltas(count);
  for (uint64_t i = 0; i < count; i++) {
    uint64_t deviation = i % 8192 == 4096 ? 1 : 0;
    deltas[i] = i + i / 32768 + deviation;
  }

  auto shift31 = OrdColumnFormat::planLinearFit(
      std::span<const uint64_t>(deltas),
      OrdColumnFormat::SINGLE_FIT_SLOPE_SHIFT);
  auto shift14 = OrdColumnFormat::planLinearFit(
      std::span<const uint64_t>(deltas),
      OrdColumnFormat::BLOCK_SLOPE_SHIFT);
  ASSERT_TRUE(shift31);
  ASSERT_TRUE(shift14);
  EXPECT_EQ(shift31->bits, 1);  // the actual 0/1 deviation
  EXPECT_GE(shift14->bits, 5);  // fixed-point drift dominates the deviation
}

TEST_F(OrdMapTest, FacetAndStringSortMatchAcrossEncodings) {
  const std::array<std::string, 8> values = {
      "hotel", "alpha", "echo", "charlie",
      "golf", "bravo", "foxtrot", "delta"};
  for (int segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int i = segment; i < (int)values.size(); i += 2) {
      docs.push_back({{"id", std::format("doc{}", i)},
                      {"flat_s", values[i]}, {"predicted_s", values[i]},
                      {"fit_s", values[i]}});
    }
    ASSERT_TRUE(helper->indexAll(docs, UpdateMessage::COMMIT).success);
  }

  auto reader = helper->getIndexWriter()->getIndexReader();
  {
    OrdMapEncodingGuard guard(OrdMap::DeltaEncoding::FLAT);
    ASSERT_NE(reader->getOrdMap("flat_s"), nullptr);
  }
  {
    OrdMapEncodingGuard guard(OrdMap::DeltaEncoding::PREDICTED);
    auto ordMap = reader->getOrdMap("predicted_s");
    ASSERT_NE(ordMap, nullptr);
    EXPECT_TRUE(ordMap->getSegToGlobal(0).predicted());
  }
  {
    OrdMapEncodingGuard guard(OrdMap::DeltaEncoding::SINGLE_FIT);
    auto ordMap = reader->getOrdMap("fit_s");
    ASSERT_NE(ordMap, nullptr);
    auto mapping = ordMap->getSegToGlobal(0);
    EXPECT_TRUE(mapping.singleFit());
    EXPECT_EQ(mapping.residualBits, 0);
    EXPECT_EQ(mapping.deltas, nullptr);
    EXPECT_EQ(mapping.blockMeta, nullptr);
  }

  struct Results {
    std::vector<std::string> sortedIds;
    std::vector<std::string> bucketIds;
    std::vector<int64_t> counts;
  };
  auto run = [&](std::string_view field) {
    auto req = localReq(helper->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").allQuery().limit(-1).fields({"id"});
    qb::sort(top, field, qb::ASC);
    top.facet("f", field).limit(-1);
    req->execute(false);
    EXPECT_OK(req);

    Results results;
    const auto* docs = req->docList("q");
    if (docs == nullptr) return results;
    const auto& ids = std::get<solux::api::ColStr>(
        docs->columns.at("id").kind).v;
    for (std::string_view id : ids) results.sortedIds.emplace_back(id);
    const auto* facet = docs->ops.at("f")->facetResult();
    const auto& buckets = std::get<solux::api::ColStr>(
        facet->bucket_ids->kind).v;
    for (std::string_view bucket : buckets) results.bucketIds.emplace_back(bucket);
    results.counts.assign(facet->counts.begin(), facet->counts.end());
    return results;
  };

  Results flat = run("flat_s");
  Results predicted = run("predicted_s");
  Results fit = run("fit_s");
  EXPECT_EQ(predicted.sortedIds, flat.sortedIds);
  EXPECT_EQ(predicted.bucketIds, flat.bucketIds);
  EXPECT_EQ(predicted.counts, flat.counts);
  EXPECT_EQ(fit.sortedIds, flat.sortedIds);
  EXPECT_EQ(fit.bucketIds, flat.bucketIds);
  EXPECT_EQ(fit.counts, flat.counts);
}
