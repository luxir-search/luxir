// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include "luxir/reader/StoredChunkCodec.h"
#include "luxir/schema/Schema.h"

namespace luxir {

// No indexing RAM charge for retained dictionaries. This mutex never nests
// another lock: callers may already hold IndexWriter's publication/index locks.
class StoredDictionaryRegistry {
public:
  struct Token {
    uint64_t epoch;
    StoredChunkFormat format;
  };

private:
  struct Slot {
    uint64_t epoch = 0;
    StoredChunkFormat format;
    std::shared_ptr<const StoredDictionary> dictionary;
    std::map<std::pair<int, uint32_t>, std::shared_ptr<const StoredCompressionDictionary>> prepared;
  };
  using Slots = std::map<std::string, Slot, std::less<>>;
  std::mutex mutex;
  Slots slots;
  uint64_t nextEpoch = 0;

  static bool samePolicy(const StoredChunkFormat& a, const StoredChunkFormat& b) {
    return a.codec == b.codec && a.dictionaryBytes == b.dictionaryBytes;
  }

public:
  using SchemaState = Slots;

  // Allocate new resource entries before manifest I/O. Applying this state
  // after the throwing publication steps only moves pointers and map nodes.
  static SchemaState prepareSchema(const Schema& schema) {
    Slots next;
    for (const auto& [name, type] : schema.fieldTypeMap) {
      if (auto* stored = dynamic_cast<const StoredFieldType*>(type.get())) {
        next[name].format = stored->format_;
      }
    }
    return next;
  }

  void setSchema(SchemaState next) {
    std::lock_guard lock(mutex);
    for (auto& [name, slot] : next) {
      auto old = slots.find(name);
      if (old != slots.end() && samePolicy(old->second.format, slot.format)) {
        slot.epoch = old->second.epoch;
        slot.dictionary = std::move(old->second.dictionary);
        slot.prepared = std::move(old->second.prepared);
      } else {
        slot.epoch = ++nextEpoch;
      }
    }
    // Removed resources disappear; re-adding one gets a fresh epoch, so an
    // old training attempt cannot publish through an A -> B -> A change.
    slots.swap(next);
  }

  std::optional<Token> token(std::string_view resource, const StoredChunkFormat& format) {
    std::lock_guard lock(mutex);
    auto it = slots.find(resource);
    if (it == slots.end() || it->second.format != format
        || format.codec != StoredChunkFormat::ZSTD || !format.dictionaryBytes) return {};
    return Token{it->second.epoch, format};
  }

  bool publish(std::string_view resource, const Token& expected,
               std::shared_ptr<const StoredDictionary> dictionary) {
    std::lock_guard lock(mutex);
    auto it = slots.find(resource);
    if (it == slots.end() || it->second.epoch != expected.epoch
        || it->second.format != expected.format || it->second.dictionary
        || !expected.format.dictionaryBytes || !dictionary || dictionary->bytes().empty()) return false;
    it->second.dictionary = std::move(dictionary);
    return true;
  }

  // Admission only needs presence, without preparing a CDict before its batch runs.
  bool hasDictionary(std::string_view resource, const StoredChunkFormat& format) {
    std::lock_guard lock(mutex);
    auto it = slots.find(resource);
    return it != slots.end() && samePolicy(it->second.format, format) && it->second.dictionary;
  }

  std::shared_ptr<const StoredCompressionDictionary> lookup(
      std::string_view resource, const StoredChunkFormat& format) {
    std::lock_guard lock(mutex);
    auto it = slots.find(resource);
    if (it == slots.end() || format.codec != StoredChunkFormat::ZSTD || !format.dictionaryBytes
        || !samePolicy(it->second.format, format) || !it->second.dictionary) return {};
    auto& slot = it->second;
    auto key = std::pair(format.zstdLevel, format.chunkTarget);
    // Keep variants pinned by writers, plus the latest requested variant.
    // Repeated level/target edits must not retain unbounded idle CDicts.
    std::erase_if(slot.prepared, [&](const auto& entry) {
      return entry.first != key && entry.second.use_count() == 1;
    });
    auto found = slot.prepared.find(key);
    if (found != slot.prepared.end()) return found->second;
    auto prepared = std::make_shared<const StoredCompressionDictionary>(slot.dictionary, format);
    slot.prepared.emplace(key, prepared);
    return prepared;
  }
};

} // namespace luxir
