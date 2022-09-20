#pragma once

#include <assert.h>

namespace solux {

/// Restores the heap invariant after the top (i.e. *begin) has been changed.
/// As in std::make_heap, this establishes a max-heap using the given less-than comparison function.
/// This can replace a std::pop_heap followed by a std::push_heap
/// Do not call on an empty heap.
template <class RandomIt, class LessCompare>
void update_heap_top(RandomIt begin, RandomIt end, LessCompare comp) {
  auto arr = begin - 1;  // 1 based array since that is how heap offsets work.
  size_t sz = end - begin + 1;  // size of our 1 based array
  assert(sz >= 1);
  size_t empty = 1;  // the index of the slot that changed (i.e. begin)
  auto newVal = std::move(arr[empty]);  // first slot was changed, so copy and empty the slot

  for(;;) {
    // calculate child indexes;
    size_t child = empty * 2;
    size_t otherChild = empty * 2 + 1;
    // find largest of the children and point "child" at it
    if (otherChild < sz && comp(arr[child], arr[otherChild])) {
      child = otherChild;
    }
    // If the new value is not smaller than the largest child, we found our spot.
    // !comp(newVal,arr[child]) was used as opposed to comp(arr[child],newVal)
    // to terminate earlier in the presence of duplicate values.
    if (child >= sz || !comp(newVal, arr[child])) {
      break;
    }
    // otherwise, the child is larger than the new val, so move it up.
    arr[empty] = std::move(arr[child]);
    empty = child;
  }

  // found the spot for the new value
  arr[empty] = std::move(newVal);
};


} // end namespace solux