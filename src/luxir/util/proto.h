// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <google/protobuf/arena.h>
#include <iterator>
#include <memory_resource>
#include <new>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luxir {

// A std::pmr view over a google::protobuf::Arena. Used to back NON-OWNING response
// message data (build-by-backing, see luxir/api/build.h): bump allocation that is
// THREAD-SAFE (the Arena permits concurrent AllocateAligned, which the parallel column
// loaders rely on) and released wholesale (deallocate is a no-op; the Arena frees on
// reset/destroy). Only trivially destructible types are stored - no cleanup is registered.
class ArenaResource : public std::pmr::memory_resource {
  google::protobuf::Arena* arena;

public:
  explicit ArenaResource(google::protobuf::Arena* a) : arena(a) {}

protected:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    return arena->AllocateAligned(bytes, alignment);
  }
  void do_deallocate(void*, std::size_t, std::size_t) override {}
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    auto* p = dynamic_cast<const ArenaResource*>(&other);
    return p != nullptr && p->arena == arena;
  }
};


constexpr bool PREALLOC_BLOCK = true;

/// Creates an arena on the heap that also has the first buffer allocated as part of that allocation.
/// use releaseArena to free it.
inline google::protobuf::Arena* createArena(size_t totalSize = 1024) {
  if constexpr (!PREALLOC_BLOCK) {
    return new google::protobuf::Arena;
  }
  else {
    // heap allocate the first buffer and the arena together
    char* buf = (char*)::operator new(totalSize);
    auto* arena = new(buf) google::protobuf::Arena(
      buf + sizeof(google::protobuf::Arena),
      totalSize - sizeof(google::protobuf::Arena));
    return arena;
  }
}

/// Frees an arena created by createArena
inline void releaseArena(google::protobuf::Arena* arena) {
  if constexpr (!PREALLOC_BLOCK) {
    delete arena;
  } else {
    arena->Reset();
    ::operator delete((char*)arena);
  }
}


/// Exception-safe replacement for google::protobuf::Arena::Create<T>, for the hand-written
/// engine objects (search ops, Query::Context, request/response wrappers) that own resources
/// and need ~T() run when the request arena is reset.
///
/// protobuf's Arena::Create<T> registers the destructor cleanup node *before* running the
/// constructor, so a ctor that throws leaves the arena scheduled to run ~T() on
/// half-constructed memory - a crash at reset, far from the throw site.  arenaCreate
/// constructs first and registers the destructor only on success (the same order protobuf
/// itself uses for map entries in CreateInArenaStorage).  If the ctor throws, C++ unwinds the
/// subobjects that were already built and the only thing "leaked" is the raw sizeof(T) bump
/// region, which the arena reclaims wholesale at reset.  So ctors of arena objects are free
/// to throw - there is no nothrow requirement.
///
/// T is placement-new'd into arena storage and never freed with delete, so `arena` must be a
/// real arena (the reference makes that a contract, not a runtime check).  Trivially
/// destructible T registers no cleanup node, matching Arena::Create.
template <class T, class... Args>
T* arenaCreate(google::protobuf::Arena& arena, Args&&... args) {
  void* p = arena.AllocateAligned(sizeof(T), alignof(T));
  T* obj = new (p) T(std::forward<Args>(args)...);
  if constexpr (!std::is_trivially_destructible_v<T>) {
    arena.OwnDestructor(obj);
  }
  return obj;
}



// Non-owning hpp-proto maps are spans over the wire bytes that PRESERVE duplicate
// keys; protobuf map semantics are last-value-wins. lastWins() collapses such a map
// to one entry per key (the LAST occurrence), in first-seen key order. The returned
// pointers alias the input map's values (same backing storage), so the map/bytes must
// outlive the result. Use wherever protobuf-map dedup semantics matter (request field
// maps, op maps, etc.).
//
// keyFn rewrites each key to the key the entry is deduplicated and emitted under, or
// returns nullopt to drop the entry entirely - so entries whose REWRITTEN keys collide
// also collapse last-wins (doc field maps under UpdateRequest.field_map).
template <class MapView, class KeyFn>
auto lastWins(const MapView& m, KeyFn&& keyFn) {
  using ValPtr = decltype(&std::begin(m)->second);  // const V*
  std::vector<std::pair<std::string_view, ValPtr>> out;
  std::unordered_map<std::string_view, size_t> pos;
  for (const auto& kv : m) {
    std::optional<std::string_view> key = keyFn(std::string_view{kv.first});
    if (!key) continue;
    auto [it, inserted] = pos.try_emplace(*key, out.size());
    if (inserted) out.emplace_back(*key, &kv.second);
    else out[it->second].second = &kv.second;  // later value wins, position preserved
  }
  return out;
}

template <class MapView>
auto lastWins(const MapView& m) {
  return lastWins(m, [](std::string_view key) { return std::optional<std::string_view>(key); });
}


} // namespace luxir
