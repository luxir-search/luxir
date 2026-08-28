#pragma once

#include <cerrno>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <sys/mman.h>

namespace luxir {

// glibc's dynamic-ratchet maximum (DEFAULT_MMAP_THRESHOLD_MAX): the
// steady-state mmap threshold when malloc is left untouched. Test binaries
// never override it; the server replaces it with its resolved mallopt value.
inline size_t mappedAllocationFloor = 32 * 1024 * 1024;

// Owns one zero-filled anonymous private mapping. Mapping sizes are rounded to
// transparent-huge-page granularity; huge-page advice and eager population are
// best-effort hints rather than allocation requirements.
class MappedAlloc {
  static constexpr size_t MAPPING_GRANULARITY = 2 * 1024 * 1024;

  void* mapping = MAP_FAILED;
  size_t mappingSize = 0;

  void reset() noexcept {
    if (mapping != MAP_FAILED) {
      (void)::munmap(mapping, mappingSize);
      mapping = MAP_FAILED;
      mappingSize = 0;
    }
  }

public:
  MappedAlloc() = default;

  explicit MappedAlloc(size_t bytes) {
    mappingSize = roundedSize(bytes);
    if (mappingSize == 0) return;
    mapping = ::mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
      mappingSize = 0;
      throw std::system_error(errno, std::generic_category(), "mmap");
    }
    (void)::madvise(mapping, mappingSize, MADV_HUGEPAGE);
    (void)::madvise(mapping, mappingSize, MADV_POPULATE_WRITE);
  }

  MappedAlloc(const MappedAlloc&) = delete;
  MappedAlloc& operator=(const MappedAlloc&) = delete;

  MappedAlloc(MappedAlloc&& other) noexcept
      : mapping(std::exchange(other.mapping, MAP_FAILED)),
        mappingSize(std::exchange(other.mappingSize, 0)) {}

  MappedAlloc& operator=(MappedAlloc&& other) noexcept {
    if (this != &other) {
      reset();
      mapping = std::exchange(other.mapping, MAP_FAILED);
      mappingSize = std::exchange(other.mappingSize, 0);
    }
    return *this;
  }

  ~MappedAlloc() { reset(); }

  static size_t roundedSize(size_t bytes) {
    if (bytes == 0) return 0;
    constexpr size_t MASK = MAPPING_GRANULARITY - 1;
    if (bytes > std::numeric_limits<size_t>::max() - MASK) {
      throw std::length_error("mapped allocation size overflow");
    }
    return (bytes + MASK) & ~MASK;
  }

  void* data() { return mapping == MAP_FAILED ? nullptr : mapping; }
  const void* data() const {
    return mapping == MAP_FAILED ? nullptr : mapping;
  }
  size_t size() const { return mappingSize; }
};

} // namespace luxir
