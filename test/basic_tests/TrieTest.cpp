#include <gtest/gtest.h>

#include <algorithm>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "solux/index/TrieBuilder.h"
#include "solux/reader/TrieReader.h"

using namespace solux;

namespace {

bool byteLess(std::string_view a, std::string_view b) {
  uint32_t n = std::min((uint32_t)a.size(), (uint32_t)b.size());
  for (uint32_t i = 0; i < n; i++) {
    uint8_t ca = (uint8_t)a[i];
    uint8_t cb = (uint8_t)b[i];
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

std::string hex(std::string_view s) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (uint8_t b : s) {
    out << std::setw(2) << (int)b;
  }
  return out.str();
}

std::string byteString(std::initializer_list<int> bytes) {
  std::string out;
  for (int b : bytes) {
    out.push_back((char)b);
  }
  return out;
}

std::vector<std::string> sortedUnique(std::vector<std::string> keys) {
  keys.erase(std::remove(keys.begin(), keys.end(), std::string()), keys.end());
  std::sort(keys.begin(), keys.end(), byteLess);
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

struct BuiltTrie {
  std::vector<char> bytes;
  uint64_t root = 0;
  int32_t nBlocks = 0;
};

BuiltTrie buildTrie(const std::vector<std::string>& keys) {
  TrieBuilder builder;
  uint32_t ord = 1;
  for (const std::string& key : keys) {
    builder.add(key, ord++);
  }

  BuiltTrie built;
  built.root = builder.finish();
  built.bytes = builder.bytes();
  built.nBlocks = (int32_t)builder.blockCount();
  return built;
}

int32_t floorRef(const std::vector<std::string>& keys, std::string_view target) {
  std::vector<std::string> all;
  all.reserve(keys.size() + 1);
  all.emplace_back();
  all.insert(all.end(), keys.begin(), keys.end());
  auto it = std::upper_bound(all.begin(), all.end(), target,
      [](std::string_view value, const std::string& key) {
        return byteLess(value, key);
      });
  return (int32_t)(it - all.begin()) - 1;
}

void expectMatchesRef(const std::vector<std::string>& keys, const std::vector<std::string>& targets) {
  BuiltTrie built = buildTrie(keys);
  TrieReader reader(built.bytes.data(), built.root, built.nBlocks);
  for (const std::string& target : targets) {
    EXPECT_EQ(reader.floorBlock(target), floorRef(keys, target)) << "target=" << hex(target);
  }
  ASSERT_GE(built.bytes.size(), 8u);
  for (uint32_t i = 0; i < 8; i++) {
    EXPECT_EQ(built.bytes[built.bytes.size() - 1 - i], 0);
  }
}

uint8_t rootSign(const BuiltTrie& built) {
  return (uint8_t)built.bytes[built.root] & 3;
}

uint8_t rootStrategy(const BuiltTrie& built) {
  return (uint8_t)built.bytes[built.root] >> 6;
}

uint32_t rootStrategyBytes(const BuiltTrie& built) {
  return (uint8_t)built.bytes[built.root + 2] + 1;
}

void expectRootStrategy(const std::vector<std::string>& keys, uint8_t strategy) {
  BuiltTrie built = buildTrie(keys);
  ASSERT_EQ(rootSign(built), TrieBuilder::SIGN_MULTI);
  EXPECT_EQ(rootStrategy(built), strategy);
}

std::vector<std::string> oneByteKeys(const std::vector<int>& labels) {
  std::vector<std::string> keys;
  for (int label : labels) {
    keys.push_back(byteString({label}));
  }
  return sortedUnique(std::move(keys));
}

void addTargetVariants(std::vector<std::string>& targets, const std::string& key) {
  targets.push_back(key);
  for (uint32_t len = 0; len <= std::min(4u, (uint32_t)key.size()); len++) {
    targets.push_back(key.substr(0, len));
  }
  if (!key.empty()) {
    uint8_t last = (uint8_t)key.back();
    if (last > 0) {
      std::string t = key;
      t.back() = (char)(last - 1);
      targets.push_back(t);
    }
    if (last < 255) {
      std::string t = key;
      t.back() = (char)(last + 1);
      targets.push_back(t);
    }
  }
  if (key.size() < 255) {
    targets.push_back(key + byteString({0}));
    targets.push_back(key + byteString({255}));
  }
}

std::vector<std::string> randomTargets(std::mt19937_64& rng, uint32_t count, uint32_t alphabet, uint32_t maxLen) {
  std::vector<std::string> targets;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t len = (uint32_t)(rng() % (maxLen + 1));
    std::string s;
    for (uint32_t j = 0; j < len; j++) {
      s.push_back((char)(rng() % alphabet));
    }
    targets.push_back(s);
  }
  return targets;
}

std::vector<std::string> makeFuzzKeys(std::mt19937_64& rng, uint32_t round) {
  if (round % 4 == 0) {
    return oneByteKeys({10, 100, 200, 250});
  }

  if (round % 4 == 1) {
    uint32_t minLabel = 16 + (uint32_t)(rng() % 80);
    std::vector<int> labels;
    for (uint32_t i = 0; i < 32; i++) {
      labels.push_back((int)(minLabel + i * 2));
    }
    return oneByteKeys(labels);
  }

  if (round % 4 == 2) {
    uint32_t minLabel = 8 + (uint32_t)(rng() % 80);
    uint32_t absentStart = minLabel + 10 + (uint32_t)(rng() % 20);
    std::vector<int> labels;
    for (uint32_t b = minLabel; b < minLabel + 64; b++) {
      if (b < absentStart || b > absentStart + 2) {
        labels.push_back((int)b);
      }
    }
    return oneByteKeys(labels);
  }

  uint32_t prefixLen = 1 + (uint32_t)(rng() % 5);
  uint32_t alphabet = 2 + (uint32_t)(rng() % 9);
  std::string prefix;
  for (uint32_t i = 0; i < prefixLen; i++) {
    prefix.push_back((char)(20 + (rng() % 80)));
  }

  std::vector<std::string> keys;
  keys.push_back(prefix);
  uint32_t count = 20 + (uint32_t)(rng() % 80);
  for (uint32_t i = 0; i < count; i++) {
    std::string s = prefix;
    uint32_t suffixLen = 1 + (uint32_t)(rng() % 8);
    for (uint32_t j = 0; j < suffixLen; j++) {
      s.push_back((char)(rng() % alphabet));
    }
    keys.push_back(s);
  }
  return sortedUnique(std::move(keys));
}

} // namespace

TEST(TrieTest, WorkedExample) {
  std::vector<std::string> keys = {"b", "bd", "bdx", "c"};
  BuiltTrie built = buildTrie(keys);
  TrieReader reader(built.bytes.data(), built.root, built.nBlocks);

  EXPECT_EQ(reader.floorBlock("bdq"), 2);
  EXPECT_EQ(reader.floorBlock("bdz"), 3);
  EXPECT_EQ(reader.floorBlock("bd"), 2);
  EXPECT_EQ(reader.floorBlock("a"), 0);
  EXPECT_EQ(reader.floorBlock("z"), 4);
  EXPECT_EQ(reader.floorBlock("bda"), 2);
  EXPECT_EQ(reader.floorBlock("bc"), 1);
}

TEST(TrieTest, FixedAdversarialCases) {
  expectMatchesRef({}, {"", "a", byteString({255})});
  expectMatchesRef({"m"}, {"", "a", "m", "n", "ma"});
  expectMatchesRef(sortedUnique({"abc", "abcx", "abd", "b"}),
      {"", "ab", "abc", "abcw", "abcx", "abcy", "abd", "b", "c"});

  std::string longKey(255, 'x');
  expectMatchesRef({longKey}, {std::string(254, 'x'), longKey, longKey + "z"});

  std::vector<std::string> binaryKeys = sortedUnique({
      byteString({0}),
      byteString({0, 255}),
      byteString({1}),
      byteString({255})
  });
  expectMatchesRef(binaryKeys, {
      "",
      byteString({0}),
      byteString({0, 128}),
      byteString({0, 255}),
      byteString({127}),
      byteString({255}),
      byteString({255, 0})
  });
}

TEST(TrieTest, StrategySelectionAndReverseBoundaries) {
  std::vector<int> bitsLabels;
  for (int i = 0; i < 32; i++) {
    bitsLabels.push_back(32 + i * 2);
  }
  expectRootStrategy(oneByteKeys(bitsLabels), TrieBuilder::STRATEGY_BITS);

  std::vector<int> multiWordBitsLabels = {20};
  for (int i = 0; i < 59; i++) {
    multiWordBitsLabels.push_back(150 + i);
  }
  std::vector<std::string> multiWordBitsKeys = oneByteKeys(multiWordBitsLabels);
  BuiltTrie multiWordBits = buildTrie(multiWordBitsKeys);
  ASSERT_EQ(rootSign(multiWordBits), TrieBuilder::SIGN_MULTI);
  EXPECT_EQ(rootStrategy(multiWordBits), TrieBuilder::STRATEGY_BITS);
  EXPECT_GT(rootStrategyBytes(multiWordBits), 16u);
  expectMatchesRef(multiWordBitsKeys, {
      byteString({21}),
      byteString({149}),
      byteString({150}),
      byteString({151}),
      byteString({208}),
      byteString({209}),
      byteString({255})
  });

  expectRootStrategy(oneByteKeys({10, 100, 200}), TrieBuilder::STRATEGY_ARRAY);

  std::vector<int> fullLabels;
  for (int i = 0; i < 256; i++) {
    fullLabels.push_back(i);
  }
  BuiltTrie full = buildTrie(oneByteKeys(fullLabels));
  ASSERT_EQ(rootSign(full), TrieBuilder::SIGN_MULTI);
  EXPECT_EQ(rootStrategy(full), TrieBuilder::STRATEGY_REVERSE_ARRAY);
  EXPECT_EQ(rootStrategyBytes(full), 1u);

  std::vector<int> reverseLabels;
  for (int i = 10; i <= 73; i++) {
    if (i < 20 || i > 22) reverseLabels.push_back(i);
  }
  std::vector<std::string> reverseKeys = oneByteKeys(reverseLabels);
  BuiltTrie reverse = buildTrie(reverseKeys);
  ASSERT_EQ(rootSign(reverse), TrieBuilder::SIGN_MULTI);
  EXPECT_EQ(rootStrategy(reverse), TrieBuilder::STRATEGY_REVERSE_ARRAY);
  EXPECT_EQ(rootStrategyBytes(reverse), 4u);
  expectMatchesRef(reverseKeys, {
      byteString({19}),
      byteString({20}),
      byteString({21}),
      byteString({22}),
      byteString({23}),
      byteString({73}),
      byteString({74})
  });
}

TEST(TrieTest, FuzzMatchesReference) {
  std::mt19937_64 rng(0x5eed1234);
  bool sawBits = false;
  bool sawArray = false;
  bool sawReverse = false;

  for (uint32_t round = 0; round < 160; round++) {
    std::vector<std::string> keys = makeFuzzKeys(rng, round);
    ASSERT_FALSE(keys.empty());

    BuiltTrie built = buildTrie(keys);
    if (rootSign(built) == TrieBuilder::SIGN_MULTI) {
      uint8_t strategy = rootStrategy(built);
      sawBits |= strategy == TrieBuilder::STRATEGY_BITS;
      sawArray |= strategy == TrieBuilder::STRATEGY_ARRAY;
      sawReverse |= strategy == TrieBuilder::STRATEGY_REVERSE_ARRAY;
    }

    std::vector<std::string> targets = randomTargets(rng, 40, 256, 12);
    targets.emplace_back();
    for (const std::string& key : keys) {
      addTargetVariants(targets, key);
    }

    TrieReader reader(built.bytes.data(), built.root, built.nBlocks);
    for (const std::string& target : targets) {
      EXPECT_EQ(reader.floorBlock(target), floorRef(keys, target))
          << "round=" << round << " target=" << hex(target);
    }
  }

  EXPECT_TRUE(sawBits);
  EXPECT_TRUE(sawArray);
  EXPECT_TRUE(sawReverse);
}
