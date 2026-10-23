// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Segment summary sidecar: builder, encoding, reader-side view and probes,
// without an index. Engine-level coverage is in SegmentSummaryIndexTest and
// SegmentPruningTest.

#include <cmath>
#include <random>
#include <gtest/gtest.h>
#include "luxir/index/SegmentSummary.h"
#include "luxir/index/SummaryCollector.h"
#include "luxir/index/SummaryInfo.h"
#include "luxir/store/Directory.h"
#include "luxir/util/geo.h"

using namespace luxir;

namespace {

using Presence = SegmentSummary::Presence;

SummaryPolicy termPolicy(SummaryPolicy::Terms terms, uint32_t setMax = 256) {
  SummaryPolicy policy;
  policy.terms = terms;
  policy.setMax = setMax;
  return policy;
}

TermKeyCollector collect(const SummaryPolicy& policy, std::span<const std::string> terms) {
  TermKeyCollector keys(policy);
  for (const auto& term : terms) keys.add(term);
  return keys;
}

std::shared_ptr<const SegmentSummary> roundTrip(SegmentSummaryBuilder& builder, uint64_t segId = 7,
                                                int32_t maxDoc = 100, bool complete = true) {
  RAMDir dir;
  FileDescriptor descriptor;
  auto written = builder.write(dir, "s07__summary_00_00", segId, maxDoc, complete, descriptor);
  EXPECT_EQ("s07__summary_00_00", descriptor.name);
  EXPECT_EQ(written->bytes(), descriptor.size);
  auto reopened = SegmentSummary::open(dir, descriptor.name);
  EXPECT_NE(nullptr, reopened);
  EXPECT_EQ(segId, reopened->segId());
  EXPECT_EQ(maxDoc, reopened->maxDoc());
  EXPECT_EQ(complete, reopened->complete());
  EXPECT_EQ(written->fields().size(), reopened->fields().size());
  return reopened;
}

} // namespace

TEST(SegmentSummary, numericBoundsAndPresence) {
  SegmentSummaryBuilder builder;
  auto& price = builder.field("price_i", FieldType::INT, FieldType::COLUMN_STORED, SummaryPolicy{});
  price.addInt64(-5);
  price.addInt64(40);
  price.setCounts(2, 2, true, false);
  auto& empty = builder.field("gone_i", FieldType::INT, FieldType::COLUMN_STORED, SummaryPolicy{});
  empty.setCounts(0, 0, true, false);
  auto summary = roundTrip(builder);

  ASSERT_EQ(nullptr, summary->field("missing_i"));
  const auto* record = summary->field("price_i");
  ASSERT_NE(nullptr, record);
  EXPECT_EQ((uint32_t)FieldType::INT, record->type);
  EXPECT_EQ((uint32_t)FieldType::COLUMN_STORED, record->flags);
  EXPECT_TRUE(record->summary_flags & SegmentSummary::HAS_NUMERIC);
  EXPECT_EQ(-5, record->num_min);
  EXPECT_EQ(40, record->num_max);
  EXPECT_EQ(Presence::MAYBE, SegmentSummary::mayIntersectNumeric(*record, 40, 50));
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*record, 41, 50));
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*record, -100, -6));
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*record, 10, 5));
  const auto* gone = summary->field("gone_i");
  ASSERT_NE(nullptr, gone);
  EXPECT_TRUE(gone->summary_flags & SegmentSummary::EMPTY);
  EXPECT_FALSE(gone->summary_flags & SegmentSummary::HAS_NUMERIC);
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*gone, 0, 0));
}

TEST(SegmentSummary, exactSetAndFilterTerms) {
  std::vector<std::string> terms = {"alpha", "beta", "gamma"};
  for (auto mode : {SummaryPolicy::Terms::SET, SummaryPolicy::Terms::FILTER, SummaryPolicy::Terms::AUTO}) {
    SegmentSummaryBuilder builder;
    SummaryPolicy policy = termPolicy(mode);
    auto& tag = builder.field("tag_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
    tag.setTermBounds(terms.front(), terms.back(), terms.size());
    tag.setTermKeys(collect(policy, terms));
    tag.setCounts(3, 3, true, false);
    auto summary = roundTrip(builder);
    const auto* record = summary->field("tag_s");
    ASSERT_NE(nullptr, record);
    EXPECT_TRUE(record->summary_flags & SegmentSummary::HAS_TERM_BOUNDS);
    for (const auto& term : terms) EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, term)) << (int)mode;
    // Outside the bounds and inside them: both proven absent, by bounds or by the set/filter.
    EXPECT_EQ(Presence::NO, summary->mayContainTerm(*record, "aaa")) << (int)mode;
    EXPECT_EQ(Presence::NO, summary->mayContainTerm(*record, "zzz")) << (int)mode;
    bool exact = mode != SummaryPolicy::Terms::FILTER;
    EXPECT_EQ(exact, summary->payload(*record, SegmentSummary::EXACT_SET) != nullptr) << (int)mode;
    EXPECT_EQ(!exact, summary->payload(*record, SegmentSummary::FUSE8) != nullptr) << (int)mode;
    if (exact) {
      EXPECT_EQ(Presence::NO, summary->mayContainTerm(*record, "beta2"));
      EXPECT_EQ(Presence::MAYBE, summary->mayContainPrefix(*record, "ga"));
      EXPECT_EQ(Presence::NO, summary->mayContainPrefix(*record, "gb"));
    }
    EXPECT_EQ(Presence::NO, summary->mayContainPrefix(*record, "zz"));
    EXPECT_FALSE(SegmentSummary::boundsExcludeTermRange(*record, std::string_view("b"), true, std::string_view("c"), true));
    EXPECT_TRUE(SegmentSummary::boundsExcludeTermRange(*record, std::string_view("gamma"), false, std::nullopt, true));
    EXPECT_TRUE(SegmentSummary::boundsExcludeTermRange(*record, std::nullopt, true, std::string_view("alpha"), false));
  }
}

TEST(SegmentSummary, setOverflowsToFilterOrNothing) {
  std::vector<std::string> terms;
  for (int i = 0; i < 300; i++) terms.push_back("t" + std::to_string(1000 + i));
  {
    SegmentSummaryBuilder builder;
    auto policy = termPolicy(SummaryPolicy::Terms::AUTO, 256);
    auto& f = builder.field("f_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
    f.setTermBounds(terms.front(), terms.back(), terms.size());
    f.setTermKeys(collect(policy, terms));
    f.setCounts(300, 300, true, false);
    auto summary = roundTrip(builder);
    const auto* record = summary->field("f_s");
    EXPECT_EQ(nullptr, summary->payload(*record, SegmentSummary::EXACT_SET));
    EXPECT_NE(nullptr, summary->payload(*record, SegmentSummary::FUSE8));
    for (const auto& term : terms) EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, term));
  }
  {
    SegmentSummaryBuilder builder;
    auto policy = termPolicy(SummaryPolicy::Terms::SET, 256);
    auto& f = builder.field("f_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
    f.setTermBounds(terms.front(), terms.back(), terms.size());
    f.setTermKeys(collect(policy, terms));
    f.setCounts(300, 300, true, false);
    auto summary = roundTrip(builder);
    const auto* record = summary->field("f_s");
    EXPECT_TRUE(record->payloads.empty());
    EXPECT_TRUE(record->summary_flags & SegmentSummary::TERMS_OVERFLOW);
    // Without a set only the bounds prove anything.
    EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, "t1150x"));
    EXPECT_EQ(Presence::NO, summary->mayContainTerm(*record, "t1500"));
    EXPECT_EQ(Presence::NO, summary->mayContainTerm(*record, "u"));
  }
  {
    // A budget too small for a fuse filter degrades to a Bloom filter.
    SegmentSummaryBuilder builder;
    auto policy = termPolicy(SummaryPolicy::Terms::FILTER);
    policy.filterMaxBytes = SummaryPolicy::MIN_FILTER_BYTES;
    std::vector<std::string> many;
    for (int i = 0; i < 20000; i++) many.push_back("k" + std::to_string(i));
    std::ranges::sort(many);
    auto& f = builder.field("f_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
    f.setTermBounds(many.front(), many.back(), many.size());
    f.setTermKeys(collect(policy, many));
    f.setCounts(20000, 20000, true, false);
    auto summary = roundTrip(builder);
    const auto* record = summary->field("f_s");
    EXPECT_NE(nullptr, summary->payload(*record, SegmentSummary::BLOOM));
    for (const auto& term : many) EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, term));
  }
}

TEST(SegmentSummary, prefixFilters) {
  SegmentSummaryBuilder builder;
  auto policy = termPolicy(SummaryPolicy::Terms::FILTER);
  policy.prefixes = {2, 4};
  std::vector<std::string> terms = {"abcd1", "abce2", "xyz"};
  auto& f = builder.field("f_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
  f.setTermBounds(terms.front(), terms.back(), terms.size());
  f.setTermKeys(collect(policy, terms));
  f.setCounts(3, 3, true, false);
  auto summary = roundTrip(builder);
  const auto* record = summary->field("f_s");
  EXPECT_NE(nullptr, summary->payload(*record, SegmentSummary::PREFIX_FUSE8, 2));
  EXPECT_NE(nullptr, summary->payload(*record, SegmentSummary::PREFIX_FUSE8, 4));
  EXPECT_EQ(Presence::MAYBE, summary->mayContainPrefix(*record, "ab"));
  EXPECT_EQ(Presence::MAYBE, summary->mayContainPrefix(*record, "abce"));
  EXPECT_EQ(Presence::MAYBE, summary->mayContainPrefix(*record, "abce2"));
  EXPECT_EQ(Presence::NO, summary->mayContainPrefix(*record, "abzz"));
  EXPECT_EQ(Presence::NO, summary->mayContainPrefix(*record, "mn"));
  // A prefix shorter than every configured length falls back to the bounds.
  EXPECT_EQ(Presence::MAYBE, summary->mayContainPrefix(*record, "b"));
}

TEST(SegmentSummary, termBoundTruncationStaysConservative) {
  std::string first(40, 'a');
  std::string last = std::string(31, 'z') + std::string(10, '\xff');
  SegmentSummaryBuilder builder;
  auto policy = termPolicy(SummaryPolicy::Terms::NONE);
  auto& f = builder.field("f_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
  f.setTermBounds(first, last, 2);
  f.setCounts(2, 2, true, false);
  auto summary = roundTrip(builder);
  const auto* record = summary->field("f_s");
  EXPECT_TRUE(record->summary_flags & SegmentSummary::TERM_BOUNDS_TRUNCATED);
  EXPECT_EQ(32u, record->term_min.size());
  EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, first));
  EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, last));
  // Truncated bounds only exclude what lies outside the truncated range: a
  // term just below the true minimum or above the true maximum stays maybe.
  EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, std::string(39, 'a')));
  EXPECT_EQ(Presence::MAYBE, summary->mayContainTerm(*record, std::string(33, 'z')));
  EXPECT_EQ(Presence::NO, summary->mayContainTerm(*record, std::string(31, 'a')));
  EXPECT_EQ(Presence::NO, summary->mayContainTerm(*record, "{"));

  SegmentSummaryBuilder unbounded;
  auto& g = unbounded.field("g_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
  g.setTermBounds("a", std::string(40, '\xff'), 2);
  g.setCounts(2, 2, true, false);
  auto other = roundTrip(unbounded);
  const auto* grecord = other->field("g_s");
  EXPECT_TRUE(grecord->summary_flags & SegmentSummary::TERM_MAX_UNBOUNDED);
  EXPECT_EQ(Presence::MAYBE, other->mayContainTerm(*grecord, std::string(50, '\xff')));
  EXPECT_EQ(Presence::NO, other->mayContainTerm(*grecord, "0"));
  EXPECT_EQ(std::nullopt, SegmentSummary::successor("\xff\xff"));
  EXPECT_EQ(std::string("b"), *SegmentSummary::successor("a\xff"));
}

TEST(SegmentSummary, geoBoxAndWrappingQueries) {
  SegmentSummaryBuilder builder;
  auto& g = builder.field("where", FieldType::GEO_POINT, FieldType::COLUMN_STORED, SummaryPolicy{});
  g.addGeoPacked(geo::encodePoint(10.0, 170.0));
  g.addGeoPacked(geo::encodePoint(20.0, 175.0));
  g.setCounts(2, 2, true, false);
  auto summary = roundTrip(builder);
  const auto* record = summary->field("where");
  ASSERT_TRUE(record->summary_flags & SegmentSummary::HAS_GEO_BOX);
  auto box = [&](double minLat, double maxLat, double minLon, double maxLon) {
    return SegmentSummary::mayIntersectGeo(*record, geo::encodeLatitude(minLat), geo::encodeLatitude(maxLat),
                                           geo::encodeLongitude(minLon), geo::encodeLongitude(maxLon));
  };
  EXPECT_EQ(Presence::MAYBE, box(15.0, 25.0, 160.0, 172.0));
  EXPECT_EQ(Presence::NO, box(30.0, 40.0, 160.0, 180.0));
  EXPECT_EQ(Presence::NO, box(10.0, 20.0, -10.0, 10.0));
  // A query box wrapping the antimeridian covers [170, 180] and [-180, -170].
  EXPECT_EQ(Presence::MAYBE, box(0.0, 30.0, 165.0, -170.0));
  EXPECT_EQ(Presence::NO, box(0.0, 30.0, 178.0, -170.0));
}

TEST(SegmentSummary, vectorCentroidCoversEveryVector) {
  std::mt19937 rng(5);
  for (uint32_t dims : {3u, 128u}) {
    std::normal_distribution<float> dist(0.0f, 2.0f);
    std::vector<std::vector<float>> vectors(500, std::vector<float>(dims));
    SegmentSummaryBuilder builder;
    auto& v = builder.field("emb_v", FieldType::VECTOR, FieldType::COLUMN_STORED | FieldType::FIXED_SIZE, SummaryPolicy{});
    for (auto& vector : vectors) {
      for (float& x : vector) x = dist(rng);
      v.addVector(vector);
    }
    v.setCounts(vectors.size(), vectors.size(), true, false);
    auto summary = roundTrip(builder);
    auto centroid = summary->vectorCentroid(*summary->field("emb_v"));
    ASSERT_TRUE(centroid.has_value());
    EXPECT_EQ(dims, centroid->dims);
    EXPECT_EQ(vectors.size(), centroid->count);
    for (const auto& vector : vectors) {
      double distance = 0.0;
      for (uint32_t d = 0; d < dims; d++) {
        double delta = (double)vector[d] - (double)centroid->centroid[d];
        distance += delta * delta;
        EXPECT_LE(centroid->boxMin[d], vector[d]);
        EXPECT_GE(centroid->boxMax[d], vector[d]);
      }
      EXPECT_LE((float)std::sqrt(distance), centroid->radius);
    }
  }
}

TEST(SegmentSummary, aggregateUnion) {
  auto make = [](int64_t min, int64_t max, bool withTag) {
    SegmentSummaryBuilder builder;
    auto& p = builder.field("price_i", FieldType::INT, FieldType::COLUMN_STORED, SummaryPolicy{});
    p.addInt64(min);
    p.addInt64(max);
    p.setCounts(2, 2, true, false);
    if (withTag) {
      auto policy = termPolicy(SummaryPolicy::Terms::SET);
      std::vector<std::string> terms = {"x"};
      auto& t = builder.field("tag_s", FieldType::STRING, FieldType::INDEX_DOCS, policy);
      t.setTermBounds("x", "x", 1);
      t.setTermKeys(collect(policy, terms));
      t.setCounts(1, 1, true, false);
    }
    return SegmentSummary::fromBytes(std::make_shared<std::vector<std::byte>>(builder.encode(1, 2, true)));
  };
  std::vector<std::shared_ptr<const SegmentSummary>> summaries = {make(5, 9, true), make(-3, 4, false)};
  bool complete = false;
  auto records = SummaryAggregator::aggregate(summaries, complete);
  EXPECT_TRUE(complete);
  ASSERT_EQ(2u, records.size());
  EXPECT_EQ("price_i", records[0].fieldname);
  EXPECT_EQ(-3, records[0].numMin);
  EXPECT_EQ(9, records[0].numMax);
  EXPECT_EQ(4u, records[0].docsWithField);
  EXPECT_EQ(2u, records[0].segmentsWithField);
  EXPECT_EQ("tag_s", records[1].fieldname);
  EXPECT_EQ(1u, records[1].segmentsWithField);
  EXPECT_TRUE(records[1].payloads.empty());
  EXPECT_EQ("x", records[1].termMin);

  summaries.push_back(nullptr);
  records = SummaryAggregator::aggregate(summaries, complete);
  EXPECT_FALSE(complete);

  std::pmr::monotonic_buffer_resource arena;
  auto wire = toWire(records, arena);
  auto view = SegmentSummary::fromAggregate(wire, true, nullptr);
  EXPECT_TRUE(view->isAggregate());
  EXPECT_NE(nullptr, view->field("price_i"));
  EXPECT_EQ(nullptr, view->field("other"));
}

TEST(SegmentSummary, malformedImagesAreRejected) {
  RAMDir dir;
  EXPECT_EQ(nullptr, SegmentSummary::open(dir, "absent"));
  SegmentSummaryBuilder builder;
  auto& p = builder.field("price_i", FieldType::INT, FieldType::COLUMN_STORED, SummaryPolicy{});
  p.addInt64(1);
  p.setCounts(1, 1, true, false);
  auto image = builder.encode(3, 1, true);
  auto truncated = std::make_shared<std::vector<std::byte>>(image.begin(), image.begin() + 12);
  EXPECT_THROW(SegmentSummary::fromBytes(truncated), std::runtime_error);
  auto corrupt = std::make_shared<std::vector<std::byte>>(image);
  (*corrupt)[0] = std::byte{'X'};
  EXPECT_THROW(SegmentSummary::fromBytes(corrupt), std::runtime_error);
}

TEST(SegmentSummary, typeFlagsRoundTripAndGateTheAggregate) {
  auto make = [](uint32_t typeFlags, int64_t min, int64_t max) {
    SegmentSummaryBuilder builder;
    auto& stamp = builder.field("stamp", FieldType::DATE, FieldType::COLUMN_STORED, SummaryPolicy{});
    stamp.typeFlags = typeFlags;
    stamp.addInt64(min);
    stamp.addInt64(max);
    stamp.setCounts(2, 2, true, false);
    return SegmentSummary::fromBytes(std::make_shared<std::vector<std::byte>>(builder.encode(1, 2, true)));
  };
  auto ns = make(1, 10, 20);
  ASSERT_NE(nullptr, ns->field("stamp"));
  EXPECT_EQ(1u, ns->field("stamp")->type_flags);
  EXPECT_EQ((uint32_t)FieldType::COLUMN_STORED, ns->field("stamp")->flags);

  bool complete = false;
  std::vector<std::shared_ptr<const SegmentSummary>> same = {ns, make(1, -5, 0)};
  auto agreed = SummaryAggregator::aggregate(same, complete);
  ASSERT_EQ(1u, agreed.size());
  EXPECT_EQ(1u, agreed[0].typeFlags);
  EXPECT_TRUE(agreed[0].summaryFlags & SegmentSummary::HAS_NUMERIC);
  EXPECT_EQ(-5, agreed[0].numMin);
  EXPECT_EQ(20, agreed[0].numMax);

  // Values in different units are not comparable: the bounds go, counts stay.
  std::vector<std::shared_ptr<const SegmentSummary>> mixed = {ns, make(0, -5, 0)};
  auto disagreed = SummaryAggregator::aggregate(mixed, complete);
  ASSERT_EQ(1u, disagreed.size());
  EXPECT_FALSE(disagreed[0].summaryFlags & SegmentSummary::HAS_NUMERIC);
  EXPECT_EQ(4u, disagreed[0].docsWithField);

  // The owned form converts losslessly through the wire form.
  std::pmr::monotonic_buffer_resource mr;
  auto back = fromWire(toWire(agreed[0], mr));
  EXPECT_EQ(agreed[0].typeFlags, back.typeFlags);
  EXPECT_EQ(agreed[0].flags, back.flags);
  EXPECT_EQ(agreed[0].summaryFlags, back.summaryFlags);
  EXPECT_EQ(agreed[0].fieldname, back.fieldname);
  EXPECT_EQ(agreed[0].nTerms, back.nTerms);
}
