#pragma once

#include "solux/util/MemPool.h"
#include "solux/util/screaming.h"
#include "solux/store/OutputStream.h"

namespace solux {

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
  ScreamingBuilder(MemPool& pool, OutputStream& out)
  : Builder(pool.alloc(screaming::BitSet::SPARSE_CONTAINER_SIZE), pool.alloc(screaming::BitSet::DENSE_CONTAINER_SIZE),
            pool.alloc(SCRATCH_SIZE), SCRATCH_SIZE), pool(pool), out(out) {
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
    return pool.alloc(scratchSize);
  }

  void deallocateScratch(void *ptr) {
    unused(ptr);
  }

};


}