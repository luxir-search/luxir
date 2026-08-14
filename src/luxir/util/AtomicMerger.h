#pragma once
#include <atomic>
#include <memory>

namespace luxir {


/// Concept that defines the requirements for a type to be used with AtomicMerger.
template <typename T>
concept Mergeable = requires(T t, T* a, T* b) {
  // Must have a 'count' member that is convertible to int64_t.
  // Number of instances that have been released (finished)
  // This count is maintained by the AtomicMerger and returned by the release() method.
  { t.releaseCount } -> std::convertible_to<int64_t>;

  // Must have a static 'merge' method that takes two T* and returns a T*.
  // The merge method should merge the two instances and return a pointer to the merged instance.
  // It can return either a or b, or a new instance.  The AtomicMerger will take care of destroying old instances.
  // This may be called concurrently with different instances of T.
  { T::merge(a, b) } -> std::same_as<T*>;
};

// Optional convenience base class for Mergeable.
class MergeableData {
public:
  int64_t releaseCount = 0;
};

/// AtomicMerger implements a thread-safe concurrent way to merge data that is collected from multiple segments.
/// It also should result in the same memory and compute performance of a single thread if a single thread
/// happens to end up processing all segments (or of the request is set to be non-parallel.)
///
/// Strategy: AtomicMerger only holds a single pointer to a Mergeable object.  On obtain(), it will
/// return a previously used instance if it is idle, or it will create a new instance.  On release(),
/// an attempt is made to return the instance of Mergeable.  If another object is already present,
/// a merge will be done and then the result will be returned to the AtomicMerger for further reuse.
///
/// NOTE: This is not necessarily algorithmically optimal merging.  For instance, if merging top scoring docs
/// per segment, a priority queue of all segment results would be computationally better. Early merging does
/// result in less memory usage though.  One could reuse this infrastructure and implement more lazy merging
/// or hybrid approaches by collecting a list of other Mergeable in a Mergeable.
///
template <Mergeable T>
class AtomicMerger {
  std::atomic<T*> ptr = nullptr;;

public:
  // allow these to be set outside constructor for greater flexibility.
  std::function<T*()> creator;
  std::function<void(T*)> destroyer;

  AtomicMerger() : AtomicMerger(
    []() { return new T; },
    [](T* data) { delete data; })  // default creator and destroyer
  {
  }

  AtomicMerger(std::function<T*()>&& creator,
               std::function<void(T*)>&& destroyer)
      : creator(std::move(creator)), destroyer(std::move(destroyer))
  {
  }

  ~AtomicMerger() {
    destroyer(ptr.load(std::memory_order_relaxed));
  }

  /// Obtain an instance of MergeableData (new or reused). The caller will have exclusive access to the data returned.
  T* obtain() {
    T* data = ptr.exchange(nullptr, std::memory_order_acquire);
    if (data == nullptr) {
      data = creator();
    }
    return data;
  }

  /// Releases the data, possibly merging it with existing data, and returns the total number of times
  /// release() has been called.
  /// Do *not* access this pointer after it has been released, as it may be deleted or merging/merged with another instance.
  int64_t release(T* data) {
    data->releaseCount++;
    for (;;) {
      auto releaseCount = data->releaseCount;  // grab the count before we try to put back, to avoid races
      data = ptr.exchange(data, std::memory_order_release);
      if (data == nullptr) {
        return releaseCount;
      }
      // try to grab the other mergeable to merge.  The memory_order_acquire will
      // also cause memory pointed at by "data" to be valid here.
      auto other = ptr.exchange(nullptr, std::memory_order_acquire);
      if (other != nullptr) {
        auto newReleaseCount = data->releaseCount + other->releaseCount;
        T* newData = nullptr;
        try {
          newData = T::merge(data, other);
        }
        catch (...) {
          // TODO: somehow communicate this failure (esp to anyone waiting for the merge to complete)?
          // Or perhaps handle exceptions at a higher level.
          destroyer(data);
          destroyer(other);
          throw;
        }
        // delete unused data
        if (newData != data) {
          destroyer(data);
        }
        if (newData != other) {
          destroyer(other);
        }
        data = newData;
        data->releaseCount = newReleaseCount; // update the count to include the merged data
      }
      // Either we successfully merged, or we got a nullptr (someone else grabbed it before us).
      // in both cases, we still have a pointer to try and put back into the atomic,
      // so continue the loop.
    }
  }

  /// Returns the current Mergeable data.  This pointer may be null if no data was obtained/released.
  /// This should not be used while processing / merging is still in progress.
  T* getData() const {
    return ptr.load(std::memory_order_acquire);
  }

};


}