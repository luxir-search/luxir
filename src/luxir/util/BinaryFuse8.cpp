// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "BinaryFuse8.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace luxir {

namespace {

inline uint64_t murmur64(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

inline uint64_t splitmix64(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

inline uint64_t mulhi(uint64_t a, uint64_t b) {
  return (uint64_t)(((__uint128_t)a * b) >> 64);
}

inline uint32_t hashIndex(int index, uint64_t hash, const BinaryFuse8::Header& header) {
  uint64_t h = mulhi(hash, header.segmentCountLength);
  h += (uint64_t)index * header.segmentLength;
  // Keep the low 36 bits; index 0 shifts them all away, 1 keeps 18, 2 keeps 36.
  uint64_t hh = hash & ((1ULL << 36) - 1);
  h ^= (hh >> (36 - 18 * index)) & header.segmentLengthMask;
  return (uint32_t)h;
}

inline uint8_t mod3(uint8_t x) { return x > 2 ? (uint8_t)(x - 3) : x; }

} // namespace

uint64_t BinaryFuse8::mix(uint64_t key, uint64_t seed) { return murmur64(key + seed); }

BinaryFuse8::Slots BinaryFuse8::slots(const Header& header, uint64_t hash) {
  uint64_t hi = mulhi(hash, header.segmentCountLength);
  Slots s;
  s.h0 = (uint32_t)hi;
  s.h1 = s.h0 + header.segmentLength;
  s.h2 = s.h1 + header.segmentLength;
  s.h1 ^= (uint32_t)(hash >> 18) & header.segmentLengthMask;
  s.h2 ^= (uint32_t)hash & header.segmentLengthMask;
  return s;
}

BinaryFuse8::Header BinaryFuse8::layout(uint32_t size) {
  constexpr uint32_t arity = 3;
  Header h{};
  // These constants are the reference implementation's; they are sensitive.
  h.segmentLength = size == 0 ? 4
      : (uint32_t)1 << (int)std::floor(std::log((double)size) / std::log(3.33) + 2.25);
  if (h.segmentLength > 262144) h.segmentLength = 262144;
  h.segmentLengthMask = h.segmentLength - 1;
  double sizeFactor = size <= 1 ? 0.0
      : std::max(1.125, 0.875 + 0.25 * std::log(1000000.0) / std::log((double)size));
  uint32_t capacity = size <= 1 ? 0 : (uint32_t)std::round((double)size * sizeFactor);
  uint32_t initSegmentCount = (capacity + h.segmentLength - 1) / h.segmentLength - (arity - 1);
  h.arrayLength = (initSegmentCount + arity - 1) * h.segmentLength;
  h.segmentCount = (h.arrayLength + h.segmentLength - 1) / h.segmentLength;
  h.segmentCount = h.segmentCount <= arity - 1 ? 1 : h.segmentCount - (arity - 1);
  h.arrayLength = (h.segmentCount + arity - 1) * h.segmentLength;
  h.segmentCountLength = h.segmentCount * h.segmentLength;
  return h;
}

size_t BinaryFuse8::sizeForKeys(size_t n) {
  if (n > UINT32_MAX) return SIZE_MAX;
  return HEADER_BYTES + layout((uint32_t)n).arrayLength;
}

bool BinaryFuse8::build(std::vector<uint64_t>& keys, std::vector<std::byte>& out,
                        uint32_t maxRetries, uint64_t seedCounter) {
  std::ranges::sort(keys);
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  if (keys.size() > UINT32_MAX) return false;
  uint32_t size = (uint32_t)keys.size();
  Header header = layout(size);
  uint32_t capacity = header.arrayLength;

  std::vector<uint64_t> reverseOrder((size_t)size + 1);
  std::vector<uint32_t> alone(capacity);
  std::vector<uint8_t> t2count(capacity);
  std::vector<uint8_t> reverseH(size);
  std::vector<uint64_t> t2hash(capacity);
  uint32_t blockBits = 1;
  while (((uint32_t)1 << blockBits) < header.segmentCount) blockBits++;
  uint32_t block = (uint32_t)1 << blockBits;
  std::vector<uint32_t> startPos(block);
  uint32_t h012[5];

  reverseOrder[size] = 1;
  uint32_t stacksize = 0;
  bool success = false;
  for (uint32_t loop = 0; loop < maxRetries; loop++) {
    header.seed = splitmix64(seedCounter);
    for (uint32_t i = 0; i < block; i++) startPos[i] = (uint32_t)(((uint64_t)i * size) >> blockBits);
    uint64_t maskblock = block - 1;
    for (uint32_t i = 0; i < size; i++) {
      uint64_t hash = mix(keys[i], header.seed);
      uint64_t segmentIndex = hash >> (64 - blockBits);
      // A zero hash is indistinguishable from an empty slot; it may be
      // misplaced, which only costs a retry.
      while (reverseOrder[startPos[segmentIndex]] != 0) {
        segmentIndex++;
        segmentIndex &= maskblock;
      }
      reverseOrder[startPos[segmentIndex]] = hash;
      startPos[segmentIndex]++;
    }
    bool error = false;
    for (uint32_t i = 0; i < size; i++) {
      uint64_t hash = reverseOrder[i];
      uint32_t h0 = hashIndex(0, hash, header);
      t2count[h0] += 4;
      t2hash[h0] ^= hash;
      uint32_t h1 = hashIndex(1, hash, header);
      t2count[h1] += 4;
      t2count[h1] ^= 1;
      t2hash[h1] ^= hash;
      uint32_t h2 = hashIndex(2, hash, header);
      t2count[h2] += 4;
      t2hash[h2] ^= hash;
      t2count[h2] ^= 2;
      // Keys are distinct, so equal hashes here are a 64-bit collision; the
      // peeling below then fails to drain and we retry with another seed.
      if (t2count[h0] < 4 || t2count[h1] < 4 || t2count[h2] < 4) error = true;
    }
    if (!error) {
      uint32_t qsize = 0;
      for (uint32_t i = 0; i < capacity; i++) {
        alone[qsize] = i;
        qsize += ((t2count[i] >> 2) == 1) ? 1 : 0;
      }
      stacksize = 0;
      while (qsize > 0) {
        qsize--;
        uint32_t index = alone[qsize];
        if ((t2count[index] >> 2) != 1) continue;
        uint64_t hash = t2hash[index];
        h012[1] = hashIndex(1, hash, header);
        h012[2] = hashIndex(2, hash, header);
        h012[3] = hashIndex(0, hash, header);
        h012[4] = h012[1];
        uint8_t found = t2count[index] & 3;
        reverseH[stacksize] = found;
        reverseOrder[stacksize] = hash;
        stacksize++;
        uint32_t other1 = h012[found + 1];
        alone[qsize] = other1;
        qsize += ((t2count[other1] >> 2) == 2) ? 1 : 0;
        t2count[other1] -= 4;
        t2count[other1] ^= mod3((uint8_t)(found + 1));
        t2hash[other1] ^= hash;
        uint32_t other2 = h012[found + 2];
        alone[qsize] = other2;
        qsize += ((t2count[other2] >> 2) == 2) ? 1 : 0;
        t2count[other2] -= 4;
        t2count[other2] ^= mod3((uint8_t)(found + 2));
        t2hash[other2] ^= hash;
      }
      if (stacksize == size) {
        success = true;
        break;
      }
    }
    std::fill(reverseOrder.begin(), reverseOrder.begin() + size, 0);
    std::ranges::fill(t2count, 0);
    std::ranges::fill(t2hash, 0);
  }
  if (!success) return false;

  size_t base = out.size();
  out.resize(base + HEADER_BYTES + capacity);
  std::memcpy(out.data() + base, &header, HEADER_BYTES);
  auto* fingerprints = (uint8_t*)(out.data() + base + HEADER_BYTES);
  std::memset(fingerprints, 0, capacity);
  for (uint32_t i = size; i-- > 0;) {
    uint64_t hash = reverseOrder[i];
    uint8_t xor2 = fingerprint(hash);
    uint8_t found = reverseH[i];
    h012[0] = hashIndex(0, hash, header);
    h012[1] = hashIndex(1, hash, header);
    h012[2] = hashIndex(2, hash, header);
    h012[3] = h012[0];
    h012[4] = h012[1];
    fingerprints[h012[found]] = (uint8_t)(xor2 ^ fingerprints[h012[found + 1]] ^ fingerprints[h012[found + 2]]);
  }
  return true;
}

std::optional<BinaryFuse8::Header> BinaryFuse8::header(std::span<const std::byte> payload) {
  if (payload.size() < HEADER_BYTES) return std::nullopt;
  Header h;
  std::memcpy(&h, payload.data(), HEADER_BYTES);
  if (h.segmentLength == 0 || (h.segmentLength & (h.segmentLength - 1)) != 0
      || h.segmentLengthMask != h.segmentLength - 1
      || h.segmentCountLength != h.segmentCount * h.segmentLength
      || h.arrayLength != (h.segmentCount + 2) * h.segmentLength
      || payload.size() < HEADER_BYTES + (size_t)h.arrayLength) {
    return std::nullopt;
  }
  return h;
}

bool BinaryFuse8::mayContain(std::span<const std::byte> payload, uint64_t key) {
  auto h = header(payload);
  if (!h) return true;
  uint64_t hash = mix(key, h->seed);
  uint8_t f = fingerprint(hash);
  Slots s = slots(*h, hash);
  auto* fingerprints = (const uint8_t*)(payload.data() + HEADER_BYTES);
  f ^= fingerprints[s.h0] ^ fingerprints[s.h1] ^ fingerprints[s.h2];
  return f == 0;
}

std::pair<uint64_t, uint64_t> BinaryFuse8::probeWindow(const Header& header, uint64_t key) {
  Slots s = slots(header, mix(key, header.seed));
  uint32_t lo = std::min({s.h0, s.h1, s.h2});
  uint32_t hi = std::max({s.h0, s.h1, s.h2});
  return {HEADER_BYTES + lo, HEADER_BYTES + hi + 1};
}

} // namespace luxir
