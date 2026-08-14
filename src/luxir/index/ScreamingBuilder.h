#pragma once

#include "luxir/util/MemPool.h"
#include "luxir/util/screaming.h"
#include "luxir/store/OutputStream.h"

namespace luxir {

// Screaming BitSet builder that writes to an OutputStream and allocates extra needed memory from a MemPool.
// No deallocations of scratch space are done.  The pool should be rolled back
// after this builder has finished.
class ScreamingBuilder : public screaming::Builder<ScreamingBuilder> {
  friend class Builder;

  MemPool& pool;
  OutputStream& out;

  // Serialized sets must start 8-aligned (screaming::BitSet::BLOCK_ALIGN):
  // dense-bucket word scans are compiled with alignment assumptions, so a
  // byte-misaligned set faults rather than merely slowing down.  Pad the
  // stream up front; the stream offset equals the mmap-relative address.
  void alignOut() {
    static constexpr char zeros[screaming::BitSet::BLOCK_ALIGN] = {};
    size_t pad = (screaming::BitSet::BLOCK_ALIGN
                  - (out.size() & (screaming::BitSet::BLOCK_ALIGN - 1)))
                 & (screaming::BitSet::BLOCK_ALIGN - 1);
    if (pad > 0) {
      out.write(zeros, pad);
    }
  }

public:
  // 8-aligned, not bare alloc(): the dense container is addressed as uint64_t
  // words (screaming.h's BLOCK_ALIGN invariant - the vectorized word scans
  // derive aligned loads from that), and the scratch buffer holds
  // BucketDescriptors plus a trailing void* link to the next scratch block.
  ScreamingBuilder(MemPool& pool, OutputStream& out)
  : Builder(pool.alloc(screaming::BitSet::SPARSE_CONTAINER_SIZE, alignof(uint64_t)),
            pool.alloc(screaming::BitSet::DENSE_CONTAINER_SIZE, alignof(uint64_t)),
            pool.alloc(SCRATCH_SIZE, alignof(uint64_t)), SCRATCH_SIZE), pool(pool), out(out) {
    alignOut();
  }

  ScreamingBuilder(MemPool& pool, OutputStream& out, void* sparseContainer, void* denseContainer, void* scratchBuf, uint32_t scratchSize)
  : Builder(sparseContainer, denseContainer, scratchBuf, scratchSize), pool(pool), out(out) {
    alignOut();
  }

protected:
  // only for use by base class
  void writeBytes(void *ptr, uint32_t nbytes) {
    out.write((char *) ptr, nbytes);
  }

  void *allocateScratch() {
    return pool.alloc(scratchSize, alignof(uint64_t));
  }

  void deallocateScratch(void *ptr) {
    unused(ptr);
  }

};


}