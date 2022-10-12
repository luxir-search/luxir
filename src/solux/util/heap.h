#pragma once

#include <assert.h>
#include <span>

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

/// Directly using std::make_heap is error-prone when dealing with indirection, esp if you have an array of
/// pointers you want to merge and you don't want to change the ordering, and you do want to know what slot
/// the top element is in (for accessing parallel arrays)
/// \tparam T
/// \tparam Comp
template <class T, class Comp>
class IndirectPQ {
  std::span<T*> pointers;
  T** end;
  T* reference;  // a reference to the start of the original array, only used to calculate index if needed by client.

  static constexpr auto ptrcomp = [](const T* a, const T* b) { return Comp()(*a,*b); };

  void makeHeap() {
    std::make_heap(pointers.data(), end, ptrcomp);
  }
  void fillPointers(std::span<T> arr) {
    assert(pointers.size() >= arr.size());
    auto sz = size();
    for (size_t i=0; i<sz; i++) {
      pointers[i] = &arr[i];
    }
  }
public:
  /// Form a priority queue over the span of pre-filled pointers to T.  The pointers do not need to point to
  /// contiguous elements.  The size of the span is never modified, but it's pointers are swapped to make a heap.
  /// This form does not have a valid reference pointer to the original array, so indexOfTop() should not be used.
  explicit IndirectPQ(std::span<T*> pointers)
          : pointers(pointers), end(pointers.data() + pointers.size()), reference(nullptr) {
    makeHeap();
  }

  /// Form an indirect priority queue of size initialSize, and limited in capacity to pointers.size()
  explicit IndirectPQ(std::span<T*> pointers, size_t initialSize)
          : pointers(pointers), end(pointers.data() + initialSize), reference(nullptr) {
    makeHeap();
  }


  IndirectPQ(std::span<T> arr, std::span<T*> pointers, bool pointersPrefilled=false)
  : pointers(pointers), end(pointers.data() + arr.size()), reference(arr.data()) {
    auto sz = arr.size();
    if (!pointersPrefilled) {
      fillPointers(arr);
    }
    makeHeap();
  }

  // Reference to the top element.
  T& top() {
    return *pointers[0];
  }

  size_t size() {
    return end - pointers.data();
  }

  /// Index of the top element in the original array
  size_t indexOfTop() {
    return &top() - reference;
  }

  /// Call this to re-heapify after top() was modified
  void updateTop() {
    update_heap_top(pointers.data(), end, ptrcomp);
  }

  T& removeTop() {
    std::pop_heap(pointers.data(), end, ptrcomp);
    --end;
    return **end;
  }

  /// If capacity has been reached, the largest element is removed and returned (i.e. heap keeps smallest)
  /// If this is a min-heap (common in solux), then we are keeping everything larger than the offered value.
  T* insertWithOverflow(T* pointerToNewVal) {
    T* ejected = nullptr;
    if (size() < pointers.size()) {
      *end = pointerToNewVal;
      ++end;
      std::push_heap(pointers.data(), end, ptrcomp);
    } else {
      // if the priority queue is full, then we only want to insert the new value if it is
      // less than the current root.
      if (ptrcomp(pointerToNewVal, pointers[0])) {
        ejected = pointers[0];
        pointers[0] = pointerToNewVal;
        updateTop();
      } else {
        ejected = pointerToNewVal;
      }
    }
    return ejected;
  }
};


} // end namespace solux