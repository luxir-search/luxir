
#include "MemPool.h"


MemPool::MemPool() {
  nextBuffer();
}

MemPool::~MemPool() {
  for (char* ptr : buffers_) {
    delete[] ptr;
  }
}

void MemPool::nextBuffer() {
  // buffer_ = new unsigned char[BYTE_BLOCK_SIZE] {0};
  buffer_ = new char[BYTE_BLOCK_SIZE];  // not 0 initialized
  buffers_.push_back(buffer_);
  bufferIndex_++;
  assert(bufferIndex_+1 == buffers_.size());
  pos_ = 0;
  byteOffset += BYTE_BLOCK_SIZE;
}
