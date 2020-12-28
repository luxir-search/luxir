
#include "MemPool.h"


MemPool::MemPool() {
  nextBuffer();
}

MemPool::~MemPool() {
  for (char* ptr : buffers) {
    delete[] ptr;
  }
}

void MemPool::nextBuffer() {
  bufferIdx++;
  if ((uint32_t)bufferIdx < buffers.size()) {
    // reuse previously allocated block
    buffer = buffers[bufferIdx];
  } else {
    buffer = new char[BYTE_BLOCK_SIZE];  // not 0 initialized
    buffers.emplace_back(buffer);
  }
  pos = 0;
}


void MemPool::_rewind(const MemPool::save_point& savePoint, uint32_t buffersToSave) {
  // this is only called if the save point wasn't in the current buffer.
  int i= bufferIdx - 1;

  // find index of matching buffer
  while (i>=0) {
    char* buf = buffers[i];
    if (savePoint >= buf && savePoint <= buf + BYTE_BLOCK_SIZE) {
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
    char* buf = buffers.back();
    assert(scribble(buf, BYTE_BLOCK_SIZE));
    delete[] buf;
    buffers.pop_back();
  }

#ifndef NDEBUG
  // scribble up to the current position on the current block (not the whole block just to save time)
  if ((uint32_t)bufferIdx < buffers.size() && bufferIdx != i) {
    scribble(buffer, pos);
  }

  // Also scribble on other blocks we are going to keep that haven't previously been scribbled on...
  // Basically, those between bufferIndex_ and i
  for (auto j=i+1; j < bufferIdx && (uint32_t)j < buffers.size(); j++) {
    scribble(buffers[j], BYTE_BLOCK_SIZE);
  }
#endif

  bufferIdx = i;
  buffer = buffers[bufferIdx];
  assert(scribble(savePoint, ptr()-savePoint));
  pos = savePoint - buffer;
}
