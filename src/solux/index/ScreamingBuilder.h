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
public:
  ScreamingBuilder(MemPool& pool, OutputStream& out)
  : Builder(pool.allocate(8192), pool.allocate(8192), pool.allocate(SCRATCH_SIZE), SCRATCH_SIZE), pool(pool), out(out) {
  }

  ScreamingBuilder(MemPool& pool, OutputStream& out, void* buf8192_a, void* buf8192_b, void* scratchBuf, uint32_t scratchSize)
  : Builder(buf8192_a, buf8192_b, scratchBuf, scratchSize), pool(pool), out(out) {
  }

protected:
  // only for use by base class
  void writeBytes(void *ptr, uint32_t nbytes) {
    out.write((char *) ptr, nbytes);
  }

  void *allocateScratch() {
    return pool.allocate(scratchSize);
  }

  void deallocateScratch(void *ptr) {
  }

};


}