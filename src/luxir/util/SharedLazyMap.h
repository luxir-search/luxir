// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <utility>
#include <variant>
#include <boost/unordered/concurrent_flat_map.hpp>
#include "SharedLazyBuild.h"


namespace luxir {
/**
 * @brief A lazy concurrent map that uses boost::container::concurrent_flat_map
 * and tbb::task_group for work-stealing waits.  It returns values
 * of std::shared_ptr<Value>
 *
 * This map allows multiple threads to concurrently request a value for a key.
 * If the value doesn't exist, the first requesting thread creates it on its
 * own stack (the "builder"), while other threads requesting the same key
 * (the "waiters") block using tbb::task_group::wait(), which lets TBB's
 * work-stealing scheduler pick up other tasks while they are blocked.
 *
 * @tparam Key The type of the keys in the map. Must be copy-constructible,
 * copy-assignable, and comparable (e.g., via operator< or std::hash).
 * @tparam Value The map will store and return values of type std::shared_ptr<Value>.
 */
template <typename Key, typename Value>
class SharedLazyMap {
public:
  using Pointer = std::shared_ptr<Value>;

private:
  using MapVal = std::variant<Pointer, std::shared_ptr<SharedLazyBuild<Value>>>;

  // A single map is used to avoid races between looking up in two maps.
  boost::unordered::concurrent_flat_map<Key, MapVal> dataMap;

public:
  SharedLazyMap() = default;

  /**
   * Retrieves the value associated with the given key if it is already fully
   * created. Returns nullptr if the key is absent or creation is in flight.
   */
  Pointer get(const Key& key) {
    Pointer result;
    dataMap.cvisit(key, [&result](const auto& elem) {
      if (auto* p = std::get_if<Pointer>(&elem.second)) result = *p;
    });
    return result;
  }

  /**
   * Replaces `expected` with `replacement` only if the key still maps to the
   * exact expected pointer. Creation-in-flight entries never match.
   */
  bool replace(const Key& key, const Pointer& expected, Pointer replacement) {
    bool replaced = false;
    dataMap.visit(key, [&](auto& elem) {
      auto* current = std::get_if<Pointer>(&elem.second);
      if (current != nullptr && *current == expected) {
        elem.second = std::move(replacement);
        replaced = true;
      }
    });
    return replaced;
  }

  /** Inserts `value` unless the key is present or being created. Never waits. */
  bool insert(const Key& key, Pointer value) {
    return dataMap.try_emplace(key, MapVal{std::move(value)});
  }

  /** Erases the key only if it still maps to the exact expected pointer. */
  bool erase(const Key& key, const Pointer& expected) {
    return dataMap.erase_if(key, [&](const auto& elem) {
      auto* current = std::get_if<Pointer>(&elem.second);
      return current != nullptr && *current == expected;
    }) != 0;
  }

  /**
   * Retrieves the value associated with the given key, or creates it
   * if it doesn't already exist.  nullptr values are not stored in the map.
   */
  template <typename CreateFunc>
  Pointer getOrCreate(const Key& key, CreateFunc&& createFunc) {
    Pointer result;
    std::shared_ptr<SharedLazyBuild<Value>> pending;
    tbb::task_handle build;
    bool foundPointer = false;

    // Insert the build attempt before releasing the map lock.
    dataMap.try_emplace_and_cvisit(key, MapVal{},
      [&](auto& elem) {
        pending = std::make_shared<SharedLazyBuild<Value>>();
        elem.second = pending;
        // Publish the deferred build before a waiter can find it.
        build = pending->defer([&]() {
          try {
            Pointer val = std::forward<CreateFunc>(createFunc)(); // do expensive part outside of visit
            if (val) {
              dataMap.visit(key, [&val](auto& elem) {
                elem.second = val;
              });
            } else {
              dataMap.erase(key);
            }
            return val;
          }
          catch (...) {
            dataMap.erase(key);
            throw;
          }
        });
      },
      [&](const auto& elem) {
        if (auto* p = std::get_if<Pointer>(&elem.second)) {
          result = *p;
          foundPointer = true;
        } else {
          pending = std::get<std::shared_ptr<SharedLazyBuild<Value>>>(elem.second);
        }
      }
    );

    if (foundPointer) return result;
    if (build) pending->run(std::move(build));
    else pending->wait();
    // A concurrent erase after the wait leaves the key absent.
    return get(key);
  }
};

}
