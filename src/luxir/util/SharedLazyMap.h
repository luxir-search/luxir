// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <exception>
#include <memory>
#include <utility>
#include <variant>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <tbb/task_group.h>


namespace luxir {
/**
 * @brief A lazy concurrent map that uses boost::container::concurrent_flat_map
 * and tbb::task_group for work-stealing waits.  It returns values
 * of std::shared_ptr<Value>
 *
 * This map allows multiple threads to concurrently request a value for a key.
 * If the value doesn't exist, one thread will be chosen to create it
 * (the "builder" thread), while other threads requesting the same key
 * (the "waiter" threads) will block using tbb::task_group::wait().
 * This allows TBB's work-stealing scheduler to pick up other tasks
 * while the waiter threads are blocked.
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
  using MapVal = std::variant<Pointer, std::shared_ptr<tbb::task_group>>;

public:
  // A single map is used to avoid races between looking up in two maps.
  boost::unordered::concurrent_flat_map<Key, MapVal> dataMap;

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
    std::shared_ptr<tbb::task_group> tg;
    bool foundPointer = false;

    // insert with the task_group alternative.
    dataMap.try_emplace_and_cvisit(key, MapVal{},
      [&](auto& elem) {
        // LOG_DEBUG("CREATE {}", key);
        tg = std::make_shared<tbb::task_group>();
        elem.second = tg;
        // must create the task when inserting the task_group to prevent race conditions,
        // otherwise another thread could wait on the task group before we add the create task.
        // Capture by ref for everything is fine here since all callers will wait on tg.
        tg->run([&]() {
          try {
            // we aren't allowed to call dataMap methods inside another dataMap method,
            // but this is guaranteed to execute outside/after the try_emplace_and_cvisit method.
            Pointer val = std::forward<CreateFunc>(createFunc)(); // do expensive part outside of visit
            if (val) {
              dataMap.visit(key, [&val](auto& elem) {
                elem.second = std::move(val);
              });
            } else {
              dataMap.erase(key);
            }
            // created = true;
          }
          catch (std::exception& e) {
            // TODO: how should we clean up?
            // LOG_ERROR("Exception caught! {}", e.what());
            dataMap.erase(key); // remove the task_group if we failed to create the value
            throw; // rethrow the exception
          }
        });
      },
      [&](const auto& elem) {
        // LOG_DEBUG("got {}", key);
        if (auto* p = std::get_if<Pointer>(&elem.second)) {
          result = *p;
          foundPointer = true;
        } else {
          tg = std::get<std::shared_ptr<tbb::task_group>>(elem.second);
        }
      }
    );

    // If the value is here, return it.
    if (foundPointer) {
      return std::move(result);
    }

    // If the task_group is present, then wait on it.
    if (!tg) {
      return nullptr;
    }
    tg->wait();

    MapVal outVal;
    auto visited = dataMap.cvisit(key, [&outVal](const auto& elem) {
      outVal = elem.second;
    });
    // A concurrent erase after the creation wait leaves the key absent.
    (void)visited;
    if (std::holds_alternative<Pointer>(outVal)) {
      return std::get<Pointer>(outVal);
    } else {
      return nullptr;
    }
  }
};

}
