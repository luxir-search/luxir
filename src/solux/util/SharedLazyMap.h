#pragma once

#include <variant>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <tbb/task_group.h>


namespace solux {
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

  // A single map is used to avoid races between looking up in two maps.
  boost::unordered::concurrent_flat_map<Key, MapVal> dataMap;

public:
  SharedLazyMap() = default;

  /**
   * Retrieves the value associated with the given key, or creates it
   * if it doesn't already exist.
   */
  Pointer getOrCreate(const Key& key, std::function<Pointer()> createFunc) {
    MapVal mapVal;

    // insert with the task_group alternative.
    auto inserted = dataMap.try_emplace_and_cvisit(key, MapVal{},
      [&](auto& elem) {
        // LOG_DEBUG("CREATE {}", key);
        auto tg = std::make_shared<tbb::task_group>();
        elem.second = tg;
        mapVal = elem.second;
        // must create the task when inserting the task_group to prevent race conditions,
        // otherwise another thread could wait on the task group before we add the create task.
        // Capture by ref for everything is fine here since all callers will wait on tg.
        tg->run([&]() {
          try {
            // we aren't allowed to call dataMap methods inside another dataMap method,
            // but this is guaranteed to execute outside/after the try_emplace_and_cvisit method.
            Pointer val = createFunc(); // do expensive part outside of visit
            dataMap.visit(key, [&createFunc, &val](auto& elem) {
              elem.second = std::move(val);
            });
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
        mapVal = elem.second;
      }
    );

    // If the value is here, return it.
    if (std::holds_alternative<Pointer>(mapVal)) {
      return std::get<Pointer>(mapVal);
    }

    // If the task_group is present, then wait on it.
    auto tg = std::get<std::shared_ptr<tbb::task_group>>(mapVal);
    tg->wait();

    MapVal outVal;
    auto visited = dataMap.cvisit(key, [&outVal](const auto& elem) {
      outVal = elem.second;
    });
    assert(visited == 1); // We don't do removals yet.  We should put things in a loop if we do in the future.
    assert(std::holds_alternative<Pointer>(outVal));
    return std::get<Pointer>(outVal);
  }
};

}
