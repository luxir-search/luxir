#pragma once

#include <assert.h>
#include <span>
#include <numeric>

namespace solux {

/// Restores the heap invariant after the top (i.e. *begin) has been changed.
/// As in std::make_heap, this establishes a max-heap using the given less-than comparison function.
/// This can replace a std::pop_heap followed by a std::push_heap
/// Do not call on an empty heap.
/// Returns true if the heap was changed.
template <class RandomIt, class LessCompare>
bool update_heap_top(RandomIt begin, RandomIt end, LessCompare comp) {
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
  return empty != 1;
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

  static constexpr auto ptrcomp = [](T* a, T* b) { return Comp()(*a,*b); };

  void makeHeap() {
    std::make_heap(pointers.data(), end, ptrcomp);
  }
  void fillPtrs(std::span<T> arr) {
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
  IndirectPQ(std::span<T*> pointers, size_t initialSize)
          : pointers(pointers), end(pointers.data() + initialSize), reference(nullptr) {
    makeHeap();
  }

  /// Form an indirect priority queue of size initialSize, and limited in capacity to pointers.size()
  IndirectPQ(std::span<T> arr, std::span<T*> pointers, size_t initialSize)
          : pointers(pointers), end(pointers.data() + initialSize), reference(arr.data()) {
    assert(arr.size() >= pointers.size());
    assert(pointers.size() >= initialSize);
    makeHeap();
  }

  IndirectPQ(std::span<T> arr, std::span<T*> pointers, bool fillPointers=true)
  : pointers(pointers), end(pointers.data() + arr.size()), reference(arr.data()) {
    auto sz = arr.size();
    if (fillPointers) {
      fillPtrs(arr);
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

  /// Call this to re-heapify after top() was modified.
  /// Returns true if the heap was changed (i.e. false of the top element was not moved)
  bool updateTop() {
    update_heap_top(pointers.data(), end, ptrcomp);
  }

  T& removeTop() {
    std::pop_heap(pointers.data(), end, ptrcomp);
    --end;
    return **end;
  }

  /// Removes the element at the given index from the heap by swapping the end element with this element
  /// and decrementing the size.  This is O(1) but does not preserve the heap invariant.  You must call
  /// heapify() to restore the heap invariant before calling any methods that depend on it.
  T& remove(size_t idx) {
    assert(idx < size());
    std::swap(pointers[idx], pointers[size()-1]);
    --end;
    return **end;
  }

  /// restores heap invariant
  void heapify() {
    std::make_heap(pointers.data(), end, ptrcomp);
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


/// Directly using std::make_heap is error-prone when dealing with indirection, esp if you have an array of
/// elements that you don't want to change the order of (hence indirection) and you want to know the slot
/// of the original element on top (for accessing parallel arrays)
/// \tparam T
/// \tparam Comp
template <class T, class Comp, typename index_type=int>
class IndexedPQ {
  std::span<T> underlying;
  std::span<index_type> heap;
  index_type heapSize; // the current heap size

  auto comparator() {
    return [&](index_type a, index_type b) { return Comp()(underlying[a],underlying[b]); };
  }

  auto end() {
    return heap.begin() + heapSize;
  }

  void makeHeap() {
    std::make_heap(heap.begin(), end(), comparator());
  }

public:

  IndexedPQ(std::span<T> underlying, std::span<index_type> indexes, bool fillIndexes=true)
  : underlying(underlying), heap(indexes), heapSize(indexes.size())
  {
    assert(underlying.size() >= indexes.size());
    if (fillIndexes) {
      assert(underlying.size() == indexes.size());
      std::iota(indexes.begin(), indexes.end(), index_type(0));
    }
    makeHeap();
  }

  IndexedPQ(std::span<T> underlying, std::span<index_type> indexes, index_type size)
          : underlying(underlying), heap(indexes), heapSize(size)
  {
    assert(underlying.size() >= indexes.size());
    assert(heapSize <= indexes.size());
    makeHeap();
  }

  // Reference to the top element.
  T& top() {
    return underlying[heap.front()];
  }

  size_t size() {
    return heapSize;
  }

  size_t capacity() {
    return heap.size();
  }

  /// Index of the top element in the original array
  index_type indexOfTop() {
    return heap.front();
  }

  /// Call this to re-heapify after top() was modified
  void updateTop() {
    update_heap_top(heap.begin(), end(), comparator());
  }

  /// Returns the index of the removed element
  index_type removeTopIndex() {
    std::pop_heap(heap.begin(), end(), comparator());
    --heapSize;
    return heap[heapSize];  // should we return the underlying index instead?
  }

  /// Any insert will invalidate / overwrite the element returned.
  T& removeTop() {
    return underlying[removeTopIndex()];
  }

  void insert(const T& elem) {
    assert(heapSize < heap.size());
    underlying[heapSize] = elem;
    heap[heapSize] = heapSize;
    heapSize++;
    std::push_heap(heap.begin(), end(), comparator());
  }

  /// If capacity has been reached, the largest element is removed (i.e. heap keeps smallest)
  /// If this is a min-heap (common in solux), then we are keeping everything larger than the offered value.
  /// @returns true if the new element caused the previous top() to be ejected.
  bool insertWithOverflow(const T& elem) {
    if (heapSize < heap.size()) {
      insert(elem);
      return false;
    }

    // if the priority queue is full, then we only want to insert the new value if it is
    // less than the current root.
    if (Comp()(elem, top())) {
      top() = elem; // overwrite the previous top
      updateTop();
      return true;
    } else {
      return false;
    }
  }

};



} // end namespace solux