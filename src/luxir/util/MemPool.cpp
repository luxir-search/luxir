
#include <stdexcept>

#include "MemPool.h"

namespace luxir {

thread_local std::unique_ptr<MemPool> MemPool::pool;

MemPool::~MemPool() {
  for (auto i = 1u; i<buffers.size(); i++) {
    char* buf = buffers[i];
#ifndef NDEBUG
    if (i <= (size_t)bufferIdx) {
      allocSize -= bufferSize(buf);
    }
#endif
    assert(scribble(buf, bufferSize(buf)));
    delete[] buf;
  }
  assert(allocSize == STATIC_BUFFER_SIZE);
}

void MemPool::nextBuffer(size_t sz) {
  auto currSize = bufferSize(buffer);
  auto nextSize = std::min(currSize * 2, BYTE_BLOCK_SIZE);  // double the size of the next buffer
  if (sz > nextSize) {
    nextSize = std::bit_ceil(sz + HEADER_SIZE);  // round up to the next block size
  }

  if ((uint32_t)bufferIdx + 1 < buffers.size()) {
    bufferIdx++;
    buffer = buffers[bufferIdx];
    pos = HEADER_SIZE;
    auto blockSize = bufferSize(buffer);
    if (sz + HEADER_SIZE > blockSize) {
      // buffer wasn't big enough, so replace it.
      delete[] buffer;
      buffers[bufferIdx] = nullptr;
      buffer = buffers[bufferIdx] = new char[nextSize];
      storeUnaligned<uint32_t>(buffer, nextSize);
    }
    allocSize += bufferSize(buffer);
  } else {
    if (buffers.size() >= MAX_BUFFERS) {
      // Indexing paths surface this as a failed update; the config clamp on
      // indexing.max-inverter-ram-mb keeps inverters from ever getting here.
      throw std::length_error("MemPool exceeded its 4GiB addressability limit");
    }
    initNewBuffer(new char[nextSize], nextSize);  // not 0 initialized.
  }
  // for new allocations, we want to let memory checkers find reads from uninitialized memory
  // assert(scribble(buffer,BYTE_BLOCK_SIZE));
  // TODO: use asan poisoning!
}

char* MemPool::backupAlloc(size_t sz) {
  nextBuffer(sz);
  char* p = buffer + pos;
  pos += sz;
  return p;
}

char* MemPool::backupAlloc(size_t sz, size_t alignment) {
  nextBuffer(sz + alignment - 1);  // reserve enough space for the alignment
  align(alignment);
  char* p = buffer + pos;
  pos += sz;
  return p;
}

#ifdef MEMPOOL_MALLOC
void MemPool::_rewind(const MemPool::save_point& savePoint, uint32_t buffersToSave) {
  luxir::unused(buffersToSave);

  if (pointers.size() < savePoint.first) {
    // exceptions?
    std::cerr << "Error rewinding pool to allocation #" << savePoint.first << ", pool only has " << pointers.size() << std::endl;
  }
  while (pointers.size() > savePoint.first) {
    // if we knew the sizes, we could scribble on the memory too... but hopefully address sanitizer and valgrind
    // can detect what we need in separate allocations.
    pointers.pop_back();
  }
  allocated = savePoint.second;
}
#else
void MemPool::_rewind(const MemPool::save_point& savePoint, uint32_t buffersToSave) {
  // this is only called if the save point wasn't in the current buffer.
  int i = bufferIdx - 1;

  // find index of matching buffer
  while (i>=0) {
    char* buf = buffers[i];
    if (savePoint >= buf && savePoint <= buf + bufferSize(buf)) {
      break;
    }
    i--;
  }

  // We didn't find the buffer... something is corrupted!
  if (i < 0) {
    // TODO
    std::cout << "CORRUPTION!" << std::endl;
  }

  auto targetLen = i + buffersToSave + 1;

  while (buffers.size() > targetLen) {
    auto idx = buffers.size() - 1;
    char* buf = buffers.back();
    if (idx <= (size_t)bufferIdx) {
      allocSize -= bufferSize(buf);
    }
    assert(scribble(buf, bufferSize(buf)));
    delete[] buf;
    buffers.pop_back();
  }

#ifndef NDEBUG
  // scribble up to the current position on the current block (not the whole block just to save time)
  if ((uint32_t)bufferIdx < buffers.size() && bufferIdx != i) {
    scribble(buffer + HEADER_SIZE, pos - HEADER_SIZE);
  }
#endif

  for (auto j=i+1; j < bufferIdx && (uint32_t)j < buffers.size(); j++) {
    allocSize -= bufferSize(buffers[j]);
#ifndef NDEBUG
    scribble(buffers[j] + HEADER_SIZE, bufferSize(buffers[j]) - HEADER_SIZE);
#endif
  }


#ifndef NDEBUG
  if (bufferIdx == i) {
    // if we were already on this block, only scribble to the end of the used space to save time
    scribble(savePoint, ptr()-savePoint);
  } else {
    // we weren't on this block, so scribble to the end of the block
    scribble(savePoint, (buffers[i]+bufferSize(buffers[i]))-savePoint);
  }
#endif

  // We're saving the current buffer, but will move off of it, so we need to adjust the allocSize.
  if ((size_t)bufferIdx < buffers.size() && bufferIdx != i) {
    allocSize -= bufferSize(buffer);
  }

  bufferIdx = i;
  buffer = buffers[bufferIdx];
  pos = savePoint - buffer;
}
#endif

} // end namespace luxir