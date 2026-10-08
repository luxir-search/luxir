// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/ProcessAllocator.h"

#include <cstdlib>

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

} // namespace

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
