// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IdFilter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <xxhash.h>

#include "luxir/util/log.h"

namespace luxir {

namespace {

static_assert(std::endian::native == std::endian::little);

constexpr uint32_t SALT[IdFilter::BLOCK_WORDS] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                                                  0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

uint64_t blockOf(uint64_t hash, uint64_t blocks) noexcept { return ((hash >> 32) * blocks) >> 32; }

uint32_t bitOf(uint64_t hash, size_t word) noexcept { return 1u << (((uint32_t)hash * SALT[word]) >> 27); }

template <class T>
T load(const char* at) noexcept {
  T value;
  std::memcpy(&value, at, sizeof(value));
  return value;
}

template <class T>
void store(char* at, T value) noexcept { std::memcpy(at, &value, sizeof(value)); }

// Binary fuse construction and probing, after Graf and Lemire, "Binary Fuse
// Filters: Fast and Smaller Than Xor Filters" (2022) and their reference
// implementation (xor_singleheader, Apache-2.0).
namespace fuse {

struct Shape {
  uint32_t segmentLength = 0;
  uint32_t segmentCount = 0;

  uint64_t arrayLength() const noexcept { return ((uint64_t)segmentCount + 2) * segmentLength; }
};

Shape shapeFor(uint64_t keys) noexcept {
  Shape shape;
  // These constants come from the reference implementation; construction time
  // is sensitive to them.
  shape.segmentLength = keys == 0 ? 4 : 1u << (int)std::floor(std::log((double)keys) / std::log(3.33) + 2.25);
  shape.segmentLength = std::min(shape.segmentLength, IdFilter::FUSE_MAX_SEGMENT_LENGTH);
  double sizeFactor = keys <= 1 ? 0 : std::max(1.125, 0.875 + 0.25 * std::log(1000000.0) / std::log((double)keys));
  auto capacity = keys <= 1 ? 0 : (uint64_t)std::round((double)keys * sizeFactor);
  auto segments = (capacity + shape.segmentLength - 1) / shape.segmentLength;
  shape.segmentCount = segments <= 2 ? 1 : (uint32_t)(segments - 2);
  return shape;
}

uint64_t mix(uint64_t key, uint64_t seed) noexcept {
  uint64_t h = key + seed;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

uint64_t splitmix64(uint64_t& state) noexcept {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

uint32_t fingerprint(uint64_t hash) noexcept { return (uint32_t)(hash ^ (hash >> 32)); }

// The three positions of a mixed key: one per consecutive segment, starting at
// a segment chosen by the high bits.
using Positions = std::array<uint64_t, 3>;

Positions positions(uint64_t hash, const Shape& shape) noexcept {
  uint64_t start = (uint64_t)(((unsigned __int128)hash * ((uint64_t)shape.segmentCount * shape.segmentLength)) >> 64);
  uint64_t mask = shape.segmentLength - 1;
  return {start, (start + shape.segmentLength) ^ ((hash >> 18) & mask),
          (start + 2 * (uint64_t)shape.segmentLength) ^ (hash & mask)};
}

uint32_t mod3(uint32_t x) noexcept { return x > 2 ? x - 3 : x; }

// Scratch bytes for building over `keys` keys into `shape`.
uint64_t scratchBytes(uint64_t keys, const Shape& shape) noexcept {
  auto array = shape.arrayLength();
  uint32_t blockBits = 1;
  while ((1ull << blockBits) < shape.segmentCount) blockBits++;
  return array * (sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint64_t)) + (keys + 1) * sizeof(uint64_t)
         + keys * sizeof(uint8_t) + (1ull << blockBits) * sizeof(uint64_t);
}

// Fills `body` (shape.arrayLength() fingerprints of Fp) and the seed. `keys`
// may be reordered and deduplicated. False when no seed worked.
template <class Fp>
bool build(std::vector<uint64_t>& keys, const Shape& shape, char* body, uint64_t& seedOut) {
  constexpr int MAX_ITERATIONS = 100;
  auto size = keys.size();
  auto capacity = shape.arrayLength();
  std::vector<uint64_t> reverseOrder(size + 1);
  std::vector<uint32_t> alone(capacity);
  std::vector<uint8_t> t2count(capacity);
  std::vector<uint8_t> reverseH(size);
  std::vector<uint64_t> t2hash(capacity);
  uint32_t blockBits = 1;
  while ((1ull << blockBits) < shape.segmentCount) blockBits++;
  uint64_t block = 1ull << blockBits;
  std::vector<uint64_t> startPos(block);
  uint64_t rng = 0x726b2b9d438b9d4dULL;
  uint64_t seed = splitmix64(rng);
  bool deduplicated = false;

  for (int loop = 0;; loop++) {
    if (loop >= MAX_ITERATIONS) return false;
    if (loop > 0) {
      std::fill(reverseOrder.begin(), reverseOrder.end(), 0);
      std::fill(t2count.begin(), t2count.end(), 0);
      std::fill(t2hash.begin(), t2hash.end(), 0);
      seed = splitmix64(rng);
    }
    reverseOrder[size] = 1;
    for (uint64_t i = 0; i < block; i++) startPos[i] = (i * size) >> blockBits;
    // Order the keys by their starting segment, for locality. A zero is the
    // empty slot marker, so a key mixing to zero needs another seed.
    bool error = false;
    for (uint64_t key : keys) {
      uint64_t hash = mix(key, seed);
      if (hash == 0) {
        error = true;
        break;
      }
      uint64_t index = hash >> (64 - blockBits);
      while (reverseOrder[startPos[index]] != 0) index = (index + 1) & (block - 1);
      reverseOrder[startPos[index]++] = hash;
    }
    if (error) continue;

    uint64_t duplicates = 0;
    for (size_t i = 0; i < size; i++) {
      uint64_t hash = reverseOrder[i];
      auto [h0, h1, h2] = positions(hash, shape);
      t2count[h0] += 4;
      t2hash[h0] ^= hash;
      t2count[h1] += 4;
      t2count[h1] ^= 1;
      t2hash[h1] ^= hash;
      t2count[h2] += 4;
      t2count[h2] ^= 2;
      t2hash[h2] ^= hash;
      // A key added twice cancels itself out of all three slots.
      if ((t2hash[h0] & t2hash[h1] & t2hash[h2]) == 0) {
        if ((t2hash[h0] == 0 && t2count[h0] == 8) || (t2hash[h1] == 0 && t2count[h1] == 8)
            || (t2hash[h2] == 0 && t2count[h2] == 8)) {
          duplicates++;
          t2count[h0] -= 4;
          t2hash[h0] ^= hash;
          t2count[h1] -= 4;
          t2count[h1] ^= 1;
          t2hash[h1] ^= hash;
          t2count[h2] -= 4;
          t2count[h2] ^= 2;
          t2hash[h2] ^= hash;
        }
      }
      error = error || t2count[h0] < 4 || t2count[h1] < 4 || t2count[h2] < 4;
    }
    if (error) continue;

    // Peel slots that hold one key.
    uint64_t queued = 0;
    for (uint64_t i = 0; i < capacity; i++) {
      alone[queued] = (uint32_t)i;
      queued += (t2count[i] >> 2) == 1 ? 1 : 0;
    }
    uint64_t stack = 0;
    while (queued > 0) {
      uint32_t index = alone[--queued];
      if ((t2count[index] >> 2) != 1) continue;
      uint64_t hash = t2hash[index];
      auto p = positions(hash, shape);
      uint64_t h012[5] = {p[0], p[1], p[2], p[0], p[1]};
      uint8_t found = t2count[index] & 3;
      reverseH[stack] = found;
      reverseOrder[stack] = hash;
      stack++;
      for (uint32_t step = 1; step <= 2; step++) {
        auto other = h012[found + step];
        alone[queued] = (uint32_t)other;
        queued += (t2count[other] >> 2) == 2 ? 1 : 0;
        t2count[other] -= 4;
        t2count[other] ^= (uint8_t)mod3(found + step);
        t2hash[other] ^= hash;
      }
    }
    if (stack + duplicates == size) {
      size = stack;
      break;
    }
    if (duplicates > 0 && !deduplicated) {
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      size = keys.size();
      reverseOrder.resize(size + 1);
      reverseH.resize(size);
      deduplicated = true;
    }
  }

  std::memset(body, 0, capacity * sizeof(Fp));
  auto at = [&](uint64_t slot) { return body + slot * sizeof(Fp); };
  for (size_t i = size; i-- > 0;) {
    uint64_t hash = reverseOrder[i];
    auto p = positions(hash, shape);
    uint64_t h012[5] = {p[0], p[1], p[2], p[0], p[1]};
    uint8_t found = reverseH[i];
    store<Fp>(at(h012[found]), (Fp)(fingerprint(hash) ^ load<Fp>(at(h012[found + 1])) ^ load<Fp>(at(h012[found + 2]))));
  }
  seedOut = seed;
  return true;
}

template <class Fp>
bool contains(const char* body, const Shape& shape, uint64_t seed, uint64_t key) noexcept {
  uint64_t hash = mix(key, seed);
  auto p = positions(hash, shape);
  auto f = (Fp)fingerprint(hash);
  f ^= load<Fp>(body + p[0] * sizeof(Fp)) ^ load<Fp>(body + p[1] * sizeof(Fp)) ^ load<Fp>(body + p[2] * sizeof(Fp));
  return f == 0;
}

} // namespace fuse

void writeHeader(char* out, IdFilter::Kind kind, uint32_t param, uint32_t segmentLength = 0,
                 uint32_t segmentCount = 0, uint64_t seed = 0) noexcept {
  std::memset(out, 0, IdFilter::HEADER_BYTES);
  out[0] = (char)kind;
  out[1] = (char)(uint8_t)param;
  store<uint32_t>(out + 4, segmentLength);
  store<uint32_t>(out + 8, segmentCount);
  store<uint64_t>(out + 16, seed);
}

std::vector<char> encodeBloom(const std::vector<uint32_t>& words, uint32_t bitsPerKey) {
  std::vector<char> out(IdFilter::HEADER_BYTES + words.size() * sizeof(uint32_t));
  writeHeader(out.data(), IdFilter::Kind::BLOOM, bitsPerKey);
  std::memcpy(out.data() + IdFilter::HEADER_BYTES, words.data(), words.size() * sizeof(uint32_t));
  return out;
}

// Extra memory a FUSE finish() over `keys` keys holds: scratch and the output.
uint64_t fuseFinishBytes(uint64_t keys, uint32_t fingerprintBits) noexcept {
  auto shape = fuse::shapeFor(keys);
  return fuse::scratchBytes(keys, shape) + IdFilter::HEADER_BYTES + shape.arrayLength() * (fingerprintBits / 8);
}

std::atomic<uint64_t> capFallbacks{0};

void bloomAdd(std::vector<uint32_t>& words, uint64_t hash) noexcept {
  auto* block = words.data() + blockOf(hash, words.size() / IdFilter::BLOCK_WORDS) * IdFilter::BLOCK_WORDS;
  for (size_t w = 0; w < IdFilter::BLOCK_WORDS; w++) block[w] |= bitOf(hash, w);
}

} // namespace

uint64_t IdFilter::hash(std::string_view term) noexcept { return XXH3_64bits(term.data(), term.size()); }

uint64_t IdFilter::bloomBlocksFor(const Options& options, uint64_t keys) noexcept {
  auto bits = std::max<uint64_t>(keys, 1) * options.bitsPerKey;
  bits = std::max<uint64_t>(bits, (uint64_t)options.minBytes * 8);
  return std::clamp<uint64_t>((bits + BLOCK_BYTES * 8 - 1) / (BLOCK_BYTES * 8), 1, MAX_BLOCKS);
}

uint64_t IdFilter::maxFuseKeysFor(int64_t ramBudgetBytes) noexcept {
  if (ramBudgetBytes <= 0) return MAX_FUSE_KEYS;
  return std::min<uint64_t>(MAX_FUSE_KEYS, (uint64_t)ramBudgetBytes / 4 / FUSE_BUILD_BYTES_PER_KEY);
}

IdFilter::Options IdFilter::optionsFor(const Options& options, uint64_t maxKeys) noexcept {
  if (options.kind != Kind::FUSE || maxKeys <= options.maxFuseKeys) return options;
  Options bloom = options;
  bloom.kind = Kind::BLOOM;
  bloom.bitsPerKey = std::max(options.bitsPerKey, CAP_FALLBACK_BITS_PER_KEY);
  return bloom;
}

uint64_t IdFilter::fuseCapFallbacks() noexcept { return capFallbacks.load(std::memory_order_relaxed); }

uint64_t IdFilter::buildBytes(const Options& requested, uint64_t keys) noexcept {
  auto options = optionsFor(requested, keys);
  switch (options.kind) {
    case Kind::BLOOM: return bloomBlocksFor(options, keys) * BLOCK_BYTES;
    case Kind::FUSE: return keys * sizeof(uint64_t) + fuseFinishBytes(keys, options.fingerprintBits);
    default: return 0;
  }
}

std::optional<IdFilter> IdFilter::parse(std::span<const char> bytes) noexcept {
  if (bytes.size() < HEADER_BYTES) return std::nullopt;
  const char* h = bytes.data();
  IdFilter filter;
  filter.body = h + HEADER_BYTES;
  filter.bodyBytes = bytes.size() - HEADER_BYTES;
  filter.kind = (Kind)(uint8_t)h[0];
  auto param = (uint32_t)(uint8_t)h[1];
  if (h[2] || h[3] || load<uint32_t>(h + 12)) return std::nullopt;
  switch (filter.kind) {
    case Kind::BLOOM: {
      if (load<uint32_t>(h + 4) || load<uint32_t>(h + 8) || load<uint64_t>(h + 16)) return std::nullopt;
      if (!param || param > MAX_BITS_PER_KEY) return std::nullopt;
      auto body = filter.bodyBytes;
      if (!body || body % BLOCK_BYTES || body / BLOCK_BYTES > MAX_BLOCKS) return std::nullopt;
      return filter;
    }
    case Kind::FUSE: {
      if (param != 8 && param != 16) return std::nullopt;
      filter.fingerprintBits = param;
      filter.segmentLength = load<uint32_t>(h + 4);
      filter.segmentCount = load<uint32_t>(h + 8);
      filter.seed = load<uint64_t>(h + 16);
      if (filter.segmentLength < 4 || filter.segmentLength > FUSE_MAX_SEGMENT_LENGTH
          || !std::has_single_bit(filter.segmentLength) || !filter.segmentCount) {
        return std::nullopt;
      }
      fuse::Shape shape{filter.segmentLength, filter.segmentCount};
      if (shape.arrayLength() * (param / 8) != filter.bodyBytes) return std::nullopt;
      return filter;
    }
    default: return std::nullopt;
  }
}

bool IdFilter::mayContainHash(uint64_t hash) const noexcept {
  switch (kind) {
    case Kind::BLOOM: {
      auto blocks = bodyBytes / BLOCK_BYTES;
      const char* block = body + blockOf(hash, blocks) * BLOCK_BYTES;
      for (size_t w = 0; w < BLOCK_WORDS; w++) {
        if (!(load<uint32_t>(block + w * sizeof(uint32_t)) & bitOf(hash, w))) return false;
      }
      return true;
    }
    case Kind::FUSE: {
      fuse::Shape shape{segmentLength, segmentCount};
      return fingerprintBits == 8 ? fuse::contains<uint8_t>(body, shape, seed, hash)
                                  : fuse::contains<uint16_t>(body, shape, seed, hash);
    }
    default: return true;
  }
}

IdFilter::Builder::Builder(const Options& requested, uint64_t maxKeys) : options(optionsFor(requested, maxKeys)) {
  if (options.kind != requested.kind) {
    capFallbacks.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("id filter: up to {} keys is above the fuse cap of {}; building bloom at {} bits per key", maxKeys,
             requested.maxFuseKeys, options.bitsPerKey);
  }
  switch (options.kind) {
    case Kind::BLOOM: words.resize(bloomBlocksFor(options, maxKeys) * BLOCK_WORDS); break;
    case Kind::FUSE:
      if (options.fingerprintBits != 8 && options.fingerprintBits != 16) {
        throw std::invalid_argument("id filter: fuse fingerprint bits must be 8 or 16");
      }
      keys.reserve(maxKeys);
      break;
    default: throw std::invalid_argument("id filter: no kind to build");
  }
}

void IdFilter::Builder::add(uint64_t hash) {
  if (options.kind == Kind::BLOOM) bloomAdd(words, hash);
  else keys.push_back(hash);
  added++;
}

size_t IdFilter::Builder::finishBytes() const noexcept {
  return options.kind == Kind::FUSE ? fuseFinishBytes(keys.size(), options.fingerprintBits) : 0;
}

std::vector<char> IdFilter::Builder::finish() {
  if (options.kind == Kind::BLOOM) return encodeBloom(words, options.bitsPerKey);

  auto shape = fuse::shapeFor(keys.size());
  auto fpBytes = options.fingerprintBits / 8;
  std::vector<char> out(HEADER_BYTES + shape.arrayLength() * fpBytes);
  uint64_t seed = 0;
  bool built = fpBytes == 1 ? fuse::build<uint8_t>(keys, shape, out.data() + HEADER_BYTES, seed)
                            : fuse::build<uint16_t>(keys, shape, out.data() + HEADER_BYTES, seed);
  if (!built) {
    // Unique keys essentially always build; never lose a filter over it.
    std::vector<uint32_t> bloom(bloomBlocksFor(options, keys.size()) * BLOCK_WORDS);
    for (auto key : keys) bloomAdd(bloom, key);
    return encodeBloom(bloom, options.bitsPerKey);
  }
  writeHeader(out.data(), Kind::FUSE, options.fingerprintBits, shape.segmentLength, shape.segmentCount, seed);
#ifndef NDEBUG
  auto filter = parse(out);
  assert(filter);
  for (auto key : keys) assert(filter->mayContainHash(key));
#endif
  return out;
}

} // namespace luxir
