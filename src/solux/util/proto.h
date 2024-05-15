#pragma once
#include <google/protobuf/arena.h>

namespace solux {

/// Creates an arena on the heap that also has the first buffer allocated as part of that allocation.
/// use releaseArena to free it.
inline google::protobuf::Arena* createArena(size_t totalSize = 1024) {
  // heap allocate the first buffer and the arena together
  char* buf = (char*)::operator new(totalSize);
  auto* arena = new (buf) google::protobuf::Arena(
          buf+sizeof(google::protobuf::Arena),
          totalSize-sizeof(google::protobuf::Arena));
  return arena;
}

/// Frees an arena created by createArena
inline void releaseArena(google::protobuf::Arena* arena) {
  arena->Reset();
  ::operator delete((char*)arena);
}




} // namespace solux
