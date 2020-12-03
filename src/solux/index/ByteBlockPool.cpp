
#include "ByteBlockPool.h"


ByteBlockPool::ByteBlockPool() {
  nextBuffer();
}

ByteBlockPool::~ByteBlockPool() {
  for (char* ptr : buffers_) {
    delete[] ptr;
  }
}

void ByteBlockPool::nextBuffer() {
  // buffer_ = new unsigned char[BYTE_BLOCK_SIZE] {0};
  buffer_ = new char[BYTE_BLOCK_SIZE];  // not 0 initialized
  buffers_.push_back(buffer_);
  bufferIndex_++;
  assert(bufferIndex_+1 == buffers_.size());
  pos_ = 0;
  byteOffset += BYTE_BLOCK_SIZE;
}
