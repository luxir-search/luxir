// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IdFilter.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <xxhash.h>

#include "luxir/reader/Postings.h"
#include "luxir/store/FileIOException.h"
#include "luxir/store/OutputStream.h"

namespace luxir {

namespace {

constexpr uint32_t SALT[IdFilter::BLOCK_WORDS] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                                                  0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

uint64_t blockOf(uint64_t h, uint64_t blocks) noexcept { return ((h >> 32) * blocks) >> 32; }

uint32_t bitOf(uint64_t h, size_t word) noexcept { return 1u << (((uint32_t)h * SALT[word]) >> 27); }

template <typename T>
void put(std::vector<std::byte>& out, T value) {
  auto at = out.size();
  out.resize(at + sizeof(T));
  std::memcpy(out.data() + at, &value, sizeof(T));
}

} // namespace

std::string IdFilter::overlayFileName(uint64_t segId, uint64_t creatorEpoch, uint64_t writerEpoch) {
  return Postings::getSegmentOverlayFileName(segId, OVERLAY_NAME, OVERLAY_GEN, 0, creatorEpoch, writerEpoch);
}

uint64_t IdFilter::hash(std::string_view term) noexcept { return XXH3_64bits(term.data(), term.size()); }

IdFilter::Builder::Builder(uint64_t expectedKeys) {
  uint64_t bits = std::max<uint64_t>(expectedKeys, 1) * BITS_PER_KEY;
  blocks = std::max<uint64_t>(1, (bits + BLOCK_BYTES * 8 - 1) / (BLOCK_BYTES * 8));
  // The block index takes the high 32 bits of a 32x32 multiply.
  blocks = std::min<uint64_t>(blocks, UINT32_MAX);
  words = std::make_unique<std::atomic<uint32_t>[]>(blocks * BLOCK_WORDS);
  for (uint64_t i = 0; i < blocks * BLOCK_WORDS; i++) words[i].store(0, std::memory_order_relaxed);
}

void IdFilter::Builder::add(std::string_view term) noexcept {
  uint64_t h = hash(term);
  auto* block = &words[blockOf(h, blocks) * BLOCK_WORDS];
  for (size_t w = 0; w < BLOCK_WORDS; w++) block[w].fetch_or(bitOf(h, w), std::memory_order_relaxed);
  keys.fetch_add(1, std::memory_order_relaxed);
}

std::vector<std::byte> IdFilter::Builder::encode() const {
  std::vector<std::byte> out;
  out.reserve(HEADER_BYTES + blocks * BLOCK_BYTES + sizeof(uint64_t));
  out.resize(MAGIC.size());
  std::memcpy(out.data(), MAGIC.data(), MAGIC.size());
  put(out, FORMAT_VERSION);
  put(out, (uint32_t)0);
  put(out, numKeys());
  put(out, blocks);
  for (uint64_t i = 0; i < blocks * BLOCK_WORDS; i++) put(out, words[i].load(std::memory_order_relaxed));
  put(out, (uint64_t)XXH3_64bits(out.data(), out.size()));
  return out;
}

FileDescriptor IdFilter::Builder::write(Directory& dir, std::string_view fileName) const {
  auto image = encode();
  Directory::FileCreateOptions options;
  options.expectedSize = image.size();
  auto file = dir.createFile(fileName, options);
  if (!file) throw FileIOException("Failed to create id filter file " + std::string(fileName));
  OutputStream out(file.get());
  out.write((const char*)image.data(), image.size());
  out.close();
  dir.finishFile(*file);
  return file->descriptor();
}

std::shared_ptr<const IdFilter> IdFilter::open(Directory& dir, std::string_view fileName) {
  auto file = dir.openFile(fileName);
  if (!file) return nullptr;
  std::string_view data = file->read();
  return fromBytes(std::span<const std::byte>((const std::byte*)data.data(), data.size()));
}

std::shared_ptr<const IdFilter> IdFilter::fromBytes(std::span<const std::byte> image) {
  if (image.size() < HEADER_BYTES + sizeof(uint64_t)) throw std::runtime_error("id filter: file too short");
  if (std::memcmp(image.data(), MAGIC.data(), MAGIC.size()) != 0) throw std::runtime_error("id filter: bad header");
  uint32_t version;
  std::memcpy(&version, image.data() + 8, sizeof(version));
  if (version != FORMAT_VERSION) throw std::runtime_error("id filter: unknown format version " + std::to_string(version));
  auto filter = std::make_shared<IdFilter>();
  std::memcpy(&filter->keys, image.data() + 16, sizeof(uint64_t));
  std::memcpy(&filter->blocks, image.data() + 24, sizeof(uint64_t));
  if (filter->blocks == 0 || filter->blocks > UINT32_MAX
      || image.size() != HEADER_BYTES + filter->blocks * BLOCK_BYTES + sizeof(uint64_t)) {
    throw std::runtime_error("id filter: size does not match its block count");
  }
  uint64_t stored;
  std::memcpy(&stored, image.data() + image.size() - sizeof(uint64_t), sizeof(stored));
  if (stored != XXH3_64bits(image.data(), image.size() - sizeof(uint64_t))) {
    throw std::runtime_error("id filter: checksum mismatch");
  }
  filter->words.resize(filter->blocks * BLOCK_WORDS);
  std::memcpy(filter->words.data(), image.data() + HEADER_BYTES, filter->words.size() * sizeof(uint32_t));
  return filter;
}

bool IdFilter::mayContainHash(uint64_t h) const noexcept {
  const uint32_t* block = words.data() + blockOf(h, blocks) * BLOCK_WORDS;
  for (size_t w = 0; w < BLOCK_WORDS; w++) {
    uint32_t bit = bitOf(h, w);
    if ((block[w] & bit) == 0) return false;
  }
  return true;
}

} // namespace luxir
