// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/ProcessAllocator.h"
#include "luxir/util/MappedAlloc.h"
#include <cerrno>
#include <cstring>
#include <limits>

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
struct HugePageHooks {
  extent_hooks_t hooks;
  extent_hooks_t* defaults;

  explicit HugePageHooks(bool wholePurge) {
    size_t size = sizeof(defaults);
    int error = mallctl("arena.0.extent_hooks", &defaults, &size, nullptr, 0);
    if (error) throw std::system_error(error, std::generic_category(), "arena.0.extent_hooks");
    hooks = *defaults;
    hooks.alloc = allocate;
    if (wholePurge) {
      hooks.purge_forced = purge<true>;
      hooks.purge_lazy = purge<false>;
    }
  }

  template<bool forced>
  static bool purge(extent_hooks_t* hooks, void* addr, size_t size,
                    size_t offset, size_t length, unsigned arena) {
    auto* self = reinterpret_cast<HugePageHooks*>(hooks);
    auto fn = forced ? self->defaults->purge_forced : self->defaults->purge_lazy;
    auto [skip, interior] = allocator_detail::hugePageInterior((uintptr_t)addr + offset, length);
    if (!fn || !interior) return true;
    bool failed = fn(self->defaults, addr, size, offset + skip, interior, arena);
    // In 5.4, pac decay (muzzy_decay_ms=0) and arena.purge reach
    // extent_dalloc_wrapper: failed purges retain the extent with zeroed=false.
    // Reuse must zero it again; only this interior was returned to the OS.
    return failed || interior != length;
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

extent_hooks_t* hugePageHooks(bool wholePurge) {
  static auto* regular = new HugePageHooks(false);
  static auto* whole = new HugePageHooks(true);
  return wholePurge ? &whole->hooks : &regular->hooks;
}
#endif
} // namespace

AllocatorArena::AllocatorArena(const char* name, AllocatorArenaOptions options)
    : hugePages(options.hugePages || options.wholeHugePagePurge) {
#if LUXIR_JEMALLOC
  try {
    for (unsigned i = 0; i < (hugePages ? 2u : 1u); ++i) {
      extent_hooks_t* hooks = hugePages && i == 0 ? hugePageHooks(options.wholeHugePagePurge) : nullptr;
      size_t size = sizeof(arenas[i]);
      int error = mallctl("arenas.create", &arenas[i], &size,
                          hooks ? &hooks : nullptr, hooks ? sizeof(hooks) : 0);
      if (error) throw std::system_error(error, std::generic_category(), "arenas.create");
      ++arenaCount;
      auto set = [&](const char* key, auto value) {
        char control[96];
        std::snprintf(control, sizeof(control), "arena.%u.%s", arenas[i], key);
        int error = mallctl(control, nullptr, nullptr, &value, sizeof(value));
        if (error) throw std::system_error(error, std::generic_category(), control);
      };
      std::string arenaName = i == 0 ? name : std::string(name) + "-regular";
      set("name", arenaName.c_str());
      if (options.wholeHugePagePurge) set("muzzy_decay_ms", (ssize_t)0);
      if (options.dirtyDecayMs) {
        set("dirty_decay_ms", (ssize_t)*options.dirtyDecayMs);
        if (*options.dirtyDecayMs > 0) {
          // 5.4 otherwise purges coalesced extents >= 8 MiB on free, ignoring
          // the reuse window when background threads are disabled.
          set("oversize_threshold", std::numeric_limits<size_t>::max());
        }
      }
    }
    // jemalloc 5.4 assigns even explicit arenas to background workers by index
    // modulo max_background_threads. Epoch advances and deferred frees wake
    // indefinite sleepers; timed early wakes are threshold-based. The wake
    // trylock can fail: idle return is best effort, not bounded by dirty_decay_ms.
  } catch (...) {
    arenaControl("destroy");
    throw;
  }
#else
  (void)name;
#endif
}

AllocatorArena::~AllocatorArena() {
  arenaControl("destroy");
}

void AllocatorArena::arenaControl(const char* command) noexcept {
#if LUXIR_JEMALLOC
  for (unsigned i = 0; i < arenaCount; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "arena.%u.%s", arenas[i], command);
    if (mallctl(name, nullptr, nullptr, nullptr, 0)) std::abort();
  }
#else
  (void)command;
#endif
}

int AllocatorArena::allocationFlags(size_t bytes, size_t alignment) const {
#if LUXIR_JEMALLOC
  // Page alignment suppresses cache-oblivious address randomization in 5.4,
  // but NOT its extra page of extent padding. For whole-huge-page-sized
  // buffers the pad starts the next huge page; whole-page purge hooks leave
  // this fringe alone. The indexing arena keeps the default purge policy.
  bool useHugePages = hugePages && bytes != 0 && bytes % hugePageSize == 0;
  // Partial huge pages can strand resident memory outside the purged extent.
  unsigned arena = arenas[hugePages && !useHugePages ? 1 : 0];
  if (useHugePages) alignment = std::max(alignment, hugePageSize);
  return MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE | MALLOCX_ALIGN(alignment);
#else
  (void)bytes;
  (void)alignment;
  return 0;
#endif
}

void* AllocatorArena::do_allocate(size_t bytes, size_t alignment) {
  return allocateImpl(bytes, alignment, false);
}

void* AllocatorArena::allocateImpl(size_t bytes, size_t alignment, bool zero) {
#if LUXIR_JEMALLOC
  void* ptr = mallocx(std::max(bytes, (size_t)1), allocationFlags(bytes, alignment) | (zero ? MALLOCX_ZERO : 0));
#else
  void* ptr = nullptr;
  if (alignment <= alignof(std::max_align_t)) ptr = std::malloc(std::max(bytes, (size_t)1));
  else if (posix_memalign(&ptr, alignment, std::max(bytes, (size_t)1))) ptr = nullptr;
#endif
  if (!ptr) throw std::bad_alloc();
#if !LUXIR_JEMALLOC
  if (zero) std::memset(ptr, 0, bytes);
#endif
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
  arenaControl("purge");
}

std::optional<AllocatorArena::Stats> AllocatorArena::stats() const {
#if LUXIR_JEMALLOC
  uint64_t epoch = 1;
  if (mallctl("epoch", nullptr, nullptr, &epoch, sizeof(epoch))) return std::nullopt;
  Stats total{};
  for (unsigned i = 0; i < arenaCount; ++i) {
    auto read = [&](const char* stat, size_t& value) {
      char name[96];
      std::snprintf(name, sizeof(name), "stats.arenas.%u.%s", arenas[i], stat);
      size_t size = sizeof(value);
      return mallctl(name, &value, &size, nullptr, 0) == 0;
    };
    size_t small, large, resident;
    if (!read("small.allocated", small) || !read("large.allocated", large) || !read("resident", resident)) {
      return std::nullopt;
    }
    total.allocated += small + large;
    total.resident += resident;
  }
  return total;
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
  if (!state.arena) state.arena = new AllocatorArena("indexing", {.hugePages = state.hugePages});
  return *state.arena;
}

AllocatorArena& bigBufferArena() {
  static auto* arena = new AllocatorArena("big-buffers",
      {.hugePages = true, .wholeHugePagePurge = true, .dirtyDecayMs = 5000});
  return *arena;
}

MappedAlloc::MappedAlloc(size_t bytes) : mappingSize(roundedSize(bytes)) {
  if (mappingSize == 0) return;
#if LUXIR_JEMALLOC
  mapping = bigBufferArena().allocateZeroed(mappingSize, hugePageSize);
#else
  mapping = ::mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    throw std::system_error(errno, std::generic_category(), "mmap");
  }
  (void)::madvise(mapping, mappingSize, MADV_HUGEPAGE);
#endif
  (void)::madvise(mapping, mappingSize, MADV_POPULATE_WRITE);
}

void MappedAlloc::reset() noexcept {
  if (!mapping) return;
#if LUXIR_JEMALLOC
  bigBufferArena().deallocate(mapping, mappingSize, hugePageSize);
#else
  (void)::munmap(mapping, mappingSize);
#endif
  mapping = nullptr;
  mappingSize = 0;
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
  appendControl<bool>(out, "opt.background_thread");
  appendControl<bool>(out, "opt.cache_oblivious");
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

bool allocatorBackgroundThreadsEnabled() {
#if LUXIR_JEMALLOC
  bool enabled = false;
  size_t size = sizeof(enabled);
  int error = mallctl("background_thread", &enabled, &size, nullptr, 0);
  if (error && error != ENOENT) {
    throw std::system_error(error, std::generic_category(), "background_thread");
  }
  return enabled;
#else
  return false;
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
