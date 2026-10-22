// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// BitSetScorer over a serialized screaming bitset: cursor iteration and the
// word-level window fill across sparse and dense buckets.

#include "test/LuxirTest.h"
#include "luxir/index/ScreamingBuilder.h"
#include "luxir/query/BitSetScorer.h"
#include "luxir/store/OutputStream.h"

using namespace luxir;

namespace {

// Serializes a set and keeps its bytes; the scorer reads from ptrToEnd.
struct SerializedSet {
  std::vector<int32_t> members;
  std::string bytes;

  explicit SerializedSet(std::vector<int32_t> values) : members(std::move(values)) {
    MemPool pool;
    RAMFile file{""};
    OutputStream out{&file};
    ScreamingBuilder builder(pool, out);
    for (int32_t v : members) builder.add(v);
    builder.flush();
    out.close();
    bytes.resize(file.size());
    file.copyTo(bytes.data());
  }

  const void* end() const { return bytes.data() + bytes.size(); }

  std::vector<int32_t> expectedIn(int32_t min, int32_t max) const {
    std::vector<int32_t> out;
    for (int32_t v : members) if (v >= min && v < max) out.push_back(v);
    return out;
  }

  int32_t firstAtOrPast(int32_t doc) const {
    for (int32_t v : members) if (v >= doc) return v;
    return PostingsReader::END;
  }
};

std::vector<int32_t> setBits(std::span<const uint64_t> words, int32_t min) {
  std::vector<int32_t> out;
  for (size_t w = 0; w < words.size(); w++) {
    for (int bit = 0; bit < 64; bit++) {
      if (words[w] & (1ULL << bit)) out.push_back(min + (int32_t)(w * 64 + bit));
    }
  }
  return out;
}

} // namespace

TEST(BitSetScorerTest, iterationAndWindowFills) {
  // Bucket 0 dense (every third doc), bucket 1 sparse, bucket 2 empty, bucket
  // 3 dense except a gap, bucket 4 a single doc.
  std::vector<int32_t> members;
  for (int32_t v = 0; v < 65536; v += 3) members.push_back(v);
  for (int32_t v : {65536 + 5, 65536 + 64, 65536 + 65, 65536 + 65535}) members.push_back(v);
  for (int32_t v = 3 * 65536; v < 4 * 65536; v++) {
    if (v % 65536 < 1000 || v % 65536 > 2100) members.push_back(v);
  }
  members.push_back(4 * 65536 + 7);
  SerializedSet set(members);

  BitSetScorer scorer(set.end(), 1.0f);
  EXPECT_EQ(0, scorer.next());
  EXPECT_EQ(3, scorer.next());
  EXPECT_EQ(65536 + 5, scorer.advance(65535));
  EXPECT_EQ(3 * 65536, scorer.advance(65536 + 65536));
  scorer.reset();

  // Windows: aligned and unaligned starts, inside one bucket, across bucket
  // edges, over the empty bucket, and past the last member.
  struct Window { int32_t min, max; };
  for (Window w : std::initializer_list<Window>{
           {0, 4096}, {64, 100}, {65, 4096}, {65536 - 128, 65536 + 128},
           {65536 + 64, 65536 + 66}, {2 * 65536, 3 * 65536 + 64}, {3 * 65536 + 960, 3 * 65536 + 2200},
           {3 * 65536 + 961, 3 * 65536 + 2200}, {4 * 65536, 5 * 65536}, {5 * 65536, 6 * 65536}}) {
    scorer.reset();
    scorer.next();
    size_t numWords = (size_t)(w.max - w.min + 63) / 64;
    std::vector<uint64_t> words(numWords, 0);
    scorer.fillWindowBits(words, w.min, w.max);
    EXPECT_EQ(set.expectedIn(w.min, w.max), setBits(words, w.min)) << w.min << ".." << w.max;
    EXPECT_EQ(set.firstAtOrPast(w.max), scorer.docId()) << w.min << ".." << w.max;
  }

  // Consecutive windows: nothing after max is lost to the next window.
  scorer.reset();
  scorer.next();
  std::vector<uint64_t> first(64, 0), second(64, 0);
  scorer.fillWindowBits(first, 0, 4096);
  scorer.fillWindowBits(second, 4096, 8192);
  EXPECT_EQ(set.expectedIn(0, 4096), setBits(first, 0));
  EXPECT_EQ(set.expectedIn(4096, 8192), setBits(second, 4096));

  // A cursor already inside the window: members before it were consumed and
  // are not reported again, members from it on are.
  scorer.reset();
  EXPECT_EQ(66, scorer.advance(65));
  std::vector<uint64_t> partial(64, 0);
  scorer.fillWindowBits(partial, 0, 4096);
  EXPECT_EQ(set.expectedIn(66, 4096), setBits(partial, 0));
  EXPECT_EQ(4098, scorer.docId());
}
