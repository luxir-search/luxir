// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/ProcessAllocator.h"

#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>

#ifndef LUXIR_JEMALLOC
#error "CMake must define LUXIR_JEMALLOC to 0 or 1"
#endif

#ifndef LUXIR_SANITIZER
#error "CMake must define LUXIR_SANITIZER to 0 or 1"
#endif
#ifndef LUXIR_SANITIZER_NAME
#error "CMake must define LUXIR_SANITIZER_NAME"
#endif

#if LUXIR_JEMALLOC
#include <jemalloc/jemalloc.h>
#include <sstream>
#define LUXIR_STRINGIFY_IMPL(x) #x
#define LUXIR_STRINGIFY(x) LUXIR_STRINGIFY_IMPL(x)
#elif LUXIR_SANITIZER
#if __has_include(<sanitizer/allocator_interface.h>)
#include <sanitizer/allocator_interface.h>
#else
// GCC exports this API but does not ship allocator_interface.h.
#include <cstddef>
extern "C" int __sanitizer_install_malloc_and_free_hooks(
    void (*malloc_hook)(const volatile void*, size_t),
    void (*free_hook)(const volatile void*));
#endif
#elif defined(__GLIBC__)
#include <gnu/libc-version.h>
#include <malloc.h>
#endif

namespace luxir {

namespace {

#if LUXIR_JEMALLOC
template<typename T>
void appendControl(std::ostringstream& out, const char* name) {
  T value{};
  size_t size = sizeof(value);
  if (mallctl(name, &value, &size, nullptr, 0) == 0) {
    out << ' ' << name << '=' << value;
  }
}
#elif LUXIR_SANITIZER
constinit thread_local uint64_t allocatedBytes = 0;

#if defined(__clang__)
__attribute__((disable_sanitizer_instrumentation))
#else
__attribute__((no_sanitize("address", "thread", "undefined")))
#endif
void countAllocation(const volatile void*, size_t size) {
  allocatedBytes += size;
}
#endif

struct IndexingArenaState {
  std::mutex mutex;
  bool hugePages = false;
  AllocatorArena* arena = nullptr;
};

IndexingArenaState& indexingArenaState() {
  static auto* state = new IndexingArenaState;
  return *state;
}

#if LUXIR_JEMALLOC
constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;

struct HugePageHooks {
  extent_hooks_t hooks;
  extent_hooks_t* defaults;

  HugePageHooks() {
    size_t size = sizeof(defaults);
    int error = mallctl("arena.0.extent_hooks", &defaults, &size, nullptr, 0);
    if (error) throw std::system_error(error, std::generic_category(), "arena.0.extent_hooks");
    hooks = *defaults;
    hooks.alloc = allocate;
  }

  static void* allocate(extent_hooks_t* hooks, void* addr, size_t size,
                        size_t alignment, bool* zero, bool* commit, unsigned arena) {
    auto* self = reinterpret_cast<HugePageHooks*>(hooks);
    void* result = self->defaults->alloc(self->defaults, addr, size, alignment, zero, commit, arena);
    // With retain, jemalloc 5.4 calls this for the growth region before splitting
    // it, so advice also covers the retained tails reused by later allocations.
    if (result) (void)madvise(result, size, MADV_HUGEPAGE);
    return result;
  }
};

extent_hooks_t* hugePageHooks() {
  static auto* hooks = new HugePageHooks;
  return &hooks->hooks;
}
#endif
} // namespace

AllocatorArena::AllocatorArena(const char* name, bool hugePages) : hugePages(hugePages) {
#if LUXIR_JEMALLOC
  extent_hooks_t* hooks = hugePages ? hugePageHooks() : nullptr;
  size_t size = sizeof(arena);
  int error = mallctl("arenas.create", &arena, &size,
                      hooks ? &hooks : nullptr, hooks ? sizeof(hooks) : 0);
  if (error) throw std::system_error(error, std::generic_category(), "arenas.create");
  char control[64];
  std::snprintf(control, sizeof(control), "arena.%u.name", arena);
  error = mallctl(control, nullptr, nullptr, &name, sizeof(name));
  if (error) {
    std::snprintf(control, sizeof(control), "arena.%u.destroy", arena);
    (void)mallctl(control, nullptr, nullptr, nullptr, 0);
    throw std::system_error(error, std::generic_category(), "arena name");
  }
#else
  (void)name;
#endif
}

AllocatorArena::~AllocatorArena() {
#if LUXIR_JEMALLOC
  char name[64];
  std::snprintf(name, sizeof(name), "arena.%u.destroy", arena);
  if (mallctl(name, nullptr, nullptr, nullptr, 0)) std::abort();
#endif
}

int AllocatorArena::allocationFlags(size_t bytes, size_t alignment) const {
#if LUXIR_JEMALLOC
  // Page alignment suppresses cache-oblivious address randomization in 5.4,
  // but NOT its extra page of extent padding. The payload is huge-page aligned;
  // the pad page can share a huge page with a neighbor, so purging the block
  // can split that one neighbor huge page.
  if (hugePages && bytes >= HUGE_PAGE_SIZE) alignment = std::max(alignment, HUGE_PAGE_SIZE);
  return MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE | MALLOCX_ALIGN(alignment);
#else
  (void)bytes;
  (void)alignment;
  return 0;
#endif
}

void* AllocatorArena::do_allocate(size_t bytes, size_t alignment) {
#if LUXIR_JEMALLOC
  void* ptr = mallocx(std::max(bytes, (size_t)1), allocationFlags(bytes, alignment));
#else
  void* ptr = nullptr;
  if (alignment <= alignof(std::max_align_t)) ptr = std::malloc(std::max(bytes, (size_t)1));
  else if (posix_memalign(&ptr, alignment, std::max(bytes, (size_t)1))) ptr = nullptr;
#endif
  if (!ptr) throw std::bad_alloc();
  return ptr;
}

void AllocatorArena::do_deallocate(void* ptr, size_t bytes, size_t alignment) {
#if LUXIR_JEMALLOC
  sdallocx(ptr, std::max(bytes, (size_t)1), allocationFlags(bytes, alignment));
#else
  (void)bytes;
  (void)alignment;
  std::free(ptr);
#endif
}

void AllocatorArena::purge() noexcept {
#if LUXIR_JEMALLOC
  char name[64];
  std::snprintf(name, sizeof(name), "arena.%u.purge", arena);
  if (mallctl(name, nullptr, nullptr, nullptr, 0)) std::abort();
#endif
}

std::optional<AllocatorArena::Stats> AllocatorArena::stats() const {
#if LUXIR_JEMALLOC
  uint64_t epoch = 1;
  if (mallctl("epoch", nullptr, nullptr, &epoch, sizeof(epoch))) return std::nullopt;
  auto read = [this](const char* stat, size_t& value) {
    char name[96];
    std::snprintf(name, sizeof(name), "stats.arenas.%u.%s", arena, stat);
    size_t size = sizeof(value);
    return mallctl(name, &value, &size, nullptr, 0) == 0;
  };
  size_t small, large, resident;
  if (read("small.allocated", small) && read("large.allocated", large) && read("resident", resident)) {
    return Stats{small + large, resident};
  }
#endif
  return std::nullopt;
}

void configureIndexingArena(bool hugePages) {
  auto& state = indexingArenaState();
  std::lock_guard lock(state.mutex);
  if (state.arena && state.hugePages != hugePages) {
    throw std::logic_error("indexing.huge-pages must be configured before indexing arena use");
  }
  state.hugePages = hugePages;
}

AllocatorArena& indexingArena() {
  auto& state = indexingArenaState();
  std::lock_guard lock(state.mutex);
  if (!state.arena) state.arena = new AllocatorArena("indexing", state.hugePages);
  return *state.arena;
}

std::string allocatorName() {
#if LUXIR_JEMALLOC
  const char* version = "unknown";
  size_t size = sizeof(version);
  (void)mallctl("version", &version, &size, nullptr, 0);
  std::ostringstream out;
  out << "jemalloc " << version << std::boolalpha;
  appendControl<const char*>(out, "config.malloc_conf");
  appendControl<ssize_t>(out, "opt.dirty_decay_ms");
  appendControl<ssize_t>(out, "opt.muzzy_decay_ms");
  appendControl<bool>(out, "opt.disable_large_size_classes");
  appendControl<bool>(out, "config.prof");
  appendControl<bool>(out, "opt.prof");
  return out.str();
#elif LUXIR_SANITIZER
  return LUXIR_SANITIZER_NAME " sanitizer";
#elif defined(__GLIBC__)
  return std::string("glibc ") + gnu_get_libc_version();
#else
  return "system";
#endif
}

void releaseFreeMemory() {
#if LUXIR_JEMALLOC
  (void)mallctl("arena." LUXIR_STRINGIFY(MALLCTL_ARENAS_ALL) ".purge",
                nullptr, nullptr, nullptr, 0);
#elif !LUXIR_SANITIZER && defined(__GLIBC__)
  malloc_trim(0);
#endif
}

std::optional<uint64_t> threadAllocatedBytes() {
#if LUXIR_JEMALLOC
  thread_local const uint64_t* counter = [] {
    const uint64_t* p = nullptr;
    size_t size = sizeof(p);
    return mallctl("thread.allocatedp", &p, &size, nullptr, 0) == 0 ? p : nullptr;
  }();
  if (counter == nullptr) std::abort();
  return *counter;
#elif LUXIR_SANITIZER
  // The test counter's startup initializer installs this before any AllocScope.
  static const int installed = __sanitizer_install_malloc_and_free_hooks(
      countAllocation, [](const volatile void*) {});
  if (!installed) std::abort();
  return allocatedBytes;
#else
  return std::nullopt;
#endif
}

} // namespace luxir
