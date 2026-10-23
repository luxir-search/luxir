// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cmath>
#include <random>
#include <gtest/gtest.h>
#include "luxir/util/BinaryFuse8.h"
#include "luxir/util/BloomFilter.h"
#include "luxir/util/TermHash.h"
#include "luxir/query/SegmentMask.h"

using namespace luxir;

namespace {

std::vector<uint64_t> randomKeys(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<uint64_t> keys(n);
  for (auto& key : keys) key = rng();
  return keys;
}

struct FuseStats {
  double falsePositiveRate;
  double bitsPerKey;
};

FuseStats buildAndMeasure(size_t n, uint64_t seed, size_t probes) {
  auto keys = randomKeys(n, seed);
  auto distinct = keys;
  std::vector<std::byte> out;
  EXPECT_TRUE(BinaryFuse8::build(distinct, out));
  EXPECT_EQ(out.size(), BinaryFuse8::sizeForKeys(distinct.size()));
  for (auto key : keys) EXPECT_TRUE(BinaryFuse8::mayContain(out, key));
  std::mt19937_64 rng(seed ^ 0x5bd1e995);
  size_t falsePositives = 0;
  for (size_t i = 0; i < probes; i++) falsePositives += BinaryFuse8::mayContain(out, rng());
  return {(double)falsePositives / (double)probes,
          distinct.empty() ? 0.0 : (double)(out.size() * 8) / (double)distinct.size()};
}

} // namespace

TEST(TermFilter, fuseNoFalseNegativesAcrossSizes) {
  for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(10), size_t(257), size_t(1000), size_t(20000)}) {
    auto stats = buildAndMeasure(n, 42 + n, 20000);
    // 8-bit fingerprints: about 1/256 false positives regardless of size.
    EXPECT_LT(stats.falsePositiveRate, 0.012) << n;
  }
}

TEST(TermFilter, fuseSpaceAndRateAtScale) {
  auto stats = buildAndMeasure(1000000, 7, 1000000);
  EXPECT_GT(stats.falsePositiveRate, 0.003);
  EXPECT_LT(stats.falsePositiveRate, 0.005);
  // The reference reports 9.04 bits per entry at this size.
  EXPECT_LE(stats.bitsPerKey, 9.3);
}

TEST(TermFilter, fuseDeduplicatesKeys) {
  std::vector<uint64_t> keys = {5, 5, 7, 7, 7, 9};
  std::vector<std::byte> out;
  ASSERT_TRUE(BinaryFuse8::build(keys, out));
  EXPECT_EQ(3u, keys.size());
  for (uint64_t key : {5, 7, 9}) EXPECT_TRUE(BinaryFuse8::mayContain(out, key));
}

TEST(TermFilter, fuseMalformedPayloadIsUnknown) {
  std::vector<std::byte> junk(10);
  EXPECT_FALSE(BinaryFuse8::header(junk).has_value());
  EXPECT_TRUE(BinaryFuse8::mayContain(junk, 1));
  auto keys = randomKeys(1000, 1);
  std::vector<std::byte> out;
  ASSERT_TRUE(BinaryFuse8::build(keys, out));
  out.resize(out.size() - 1);
  EXPECT_FALSE(BinaryFuse8::header(out).has_value());
  EXPECT_TRUE(BinaryFuse8::mayContain(out, 123));
}

TEST(TermFilter, fuseRetriesExhausted) {
  auto keys = randomKeys(5000, 2);
  std::vector<std::byte> out;
  EXPECT_FALSE(BinaryFuse8::build(keys, out, 0));
  EXPECT_TRUE(out.empty());
}

TEST(TermFilter, fuseProbeWindowCoversSlots) {
  auto keys = randomKeys(5000, 3);
  std::vector<std::byte> out;
  ASSERT_TRUE(BinaryFuse8::build(keys, out));
  auto header = BinaryFuse8::header(out);
  ASSERT_TRUE(header.has_value());
  for (size_t i = 0; i < 1000; i++) {
    uint64_t key = keys[i];
    auto [begin, end] = BinaryFuse8::probeWindow(*header, key);
    auto slots = BinaryFuse8::slots(*header, BinaryFuse8::mix(key, header->seed));
    for (uint32_t slot : {slots.h0, slots.h1, slots.h2}) {
      EXPECT_LE(begin, BinaryFuse8::HEADER_BYTES + slot);
      EXPECT_LT(BinaryFuse8::HEADER_BYTES + slot, end);
    }
    EXPECT_LE(end, out.size());
    // A probe over just the window answers the same as the whole payload.
    EXPECT_TRUE(BinaryFuse8::mayContain(out, key));
  }
}

TEST(TermFilter, bloomWithinBudget) {
  for (double bitsPerKey : {3.0, 6.0, 10.0}) {
    size_t n = 100000;
    uint64_t budget = (uint64_t)((double)n * bitsPerKey / 8) + BloomFilter::HEADER_BYTES;
    auto plan = BloomFilter::plan(budget, n, 99);
    BloomFilter::Builder builder(plan);
    auto keys = randomKeys(n, 7);
    for (auto key : keys) builder.add(key);
    std::vector<std::byte> out;
    builder.appendTo(out);
    EXPECT_LE(out.size(), budget);
    EXPECT_EQ(n, builder.keys());
    for (auto key : keys) EXPECT_TRUE(BloomFilter::mayContain(out, key));
    std::mt19937_64 rng(11);
    size_t probes = 200000, falsePositives = 0;
    for (size_t i = 0; i < probes; i++) falsePositives += BloomFilter::mayContain(out, rng());
    double rate = (double)falsePositives / (double)probes;
    double theory = std::pow(1 - std::exp(-(double)plan.k / bitsPerKey), plan.k);
    EXPECT_LT(rate, theory * 1.5 + 0.002) << bitsPerKey;
    auto header = BloomFilter::header(out);
    ASSERT_TRUE(header.has_value());
    std::vector<uint64_t> offsets;
    BloomFilter::probeOffsets(*header, keys[0], offsets);
    EXPECT_EQ(header->k, offsets.size());
    for (uint64_t offset : offsets) EXPECT_LE(offset + sizeof(uint64_t), out.size());
  }
  std::vector<std::byte> junk(3);
  EXPECT_TRUE(BloomFilter::mayContain(junk, 1));
}

TEST(TermFilter, termKeyIsStable) {
  // Persisted in summary sidecars: a change here is a HASH_VERSION bump.
  EXPECT_EQ(1u, summary::HASH_VERSION);
  EXPECT_EQ(XXH3_64bits("", 0), summary::termKey(""));
  EXPECT_EQ(XXH3_64bits("luxir", 5), summary::termKey("luxir"));
  EXPECT_NE(summary::termKey("a"), summary::termKey("b"));
}

TEST(TermFilter, segmentMask) {
  std::vector<uint64_t> allStorage, someStorage, copyStorage, emptyStorage;
  SegmentMask all = SegmentMask::make(allStorage, 70, true);
  EXPECT_TRUE(all.isAll());
  EXPECT_EQ(70u, all.popcount());
  all.clear(69);
  EXPECT_FALSE(all.test(69));
  EXPECT_TRUE(all.test(68));
  SegmentMask some = SegmentMask::make(someStorage, 70, false);
  some.set(3);
  EXPECT_TRUE(some.subsetOf(all));
  EXPECT_FALSE(all.subsetOf(some));
  SegmentMask copy = all.copy(copyStorage);
  copy &= some;
  EXPECT_TRUE(copy.equals(some));
  size_t visited = 0;
  some.forEach([&](size_t ord) { visited += ord; });
  EXPECT_EQ(3u, visited);
  copy = all.copy(copyStorage);
  copy.subtract(some);
  EXPECT_FALSE(copy.test(3));
  EXPECT_EQ(all.popcount() - 1, copy.popcount());
  SegmentMask none = SegmentMask::make(emptyStorage, 0, true);
  EXPECT_TRUE(none.isAll());
  EXPECT_TRUE(none.isNone());
  EXPECT_NE(all.hash(), some.hash());
}
