// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <span>
#include "DocStream.h"

namespace luxir {

// Document-order term ids for indexed STRING arrays. Both streams live in the
// inverter's accounted pool; no slot table or per-doc stream allocation.
class ArrayOrdValues {
  IntStream ids;
  Stream lengths;
  int64_t docSize = 0;
  int32_t docs = 0;
  bool multi = false;

public:
  explicit ArrayOrdValues(MemPool& pool) : ids(pool) {}

  void startDoc(MemPool& pool) {
    if (docSize != 0) lengths.writeVLong(pool, docSize);
    docSize = 0;
    docs++;
  }
  void add(MemPool& pool, uint32_t id) {
    assert(id > 0 && id <= INT32_MAX);
    ids.addVal(pool, (int32_t)id);
    multi |= ++docSize > 1;
  }
  void finish(MemPool& pool) {
    assert(docSize > 0);
    lengths.writeVLong(pool, docSize);
    docSize = 0;
  }

  // OrdColWriter makes forward passes over slots. Restart the stream on each
  // pass, remapping ids in place rather than copying the sequences at flush.
  class Mapped {
    const ArrayOrdValues& source;
    const MemPool& pool;
    std::span<const uint32_t> mapping;
    mutable std::optional<IntStream::Reader> ids;
    mutable std::optional<StreamReader> lengths;
    mutable int32_t previous = -1;
  public:
    Mapped(const ArrayOrdValues& source, const MemPool& pool,
           std::span<const uint32_t> mapping) : source(source), pool(pool), mapping(mapping) {}
    int32_t size() const { return source.docs; }
    int32_t docsWithValue() const { return source.docs; }
    bool multiValued() const { return source.multi; }
    bool hasValues(int32_t) const { return true; }
    void pushValues(int32_t slot, auto&& acceptor) const {
      if (slot <= previous || !ids) {
        ids.emplace(source.ids, pool);
        lengths.emplace(source.lengths, pool);
        previous = -1;
      }
      assert(slot == previous + 1);
      int64_t count = lengths->readVlong();
      for (int64_t i = 0; i < count; i++) acceptor(mapping[(uint32_t)ids->next()]);
      previous = slot;
    }
  };
};

} // namespace luxir
