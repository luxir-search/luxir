#include "test/SoluxTest.h"
#include "solux/util/SharedLazyMap.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace solux;

class UtilTest : public SoluxTest {
protected:
};

TEST_F(UtilTest, lazyMap) {
  // test exception handling
  {
    SharedLazyMap<int, int> map;
    int count = 0;
    auto create = [&]() {
      auto v = std::make_shared<int>(42);
      count++;
      if (count % 2 == 1) {
        throw std::runtime_error("Test exception");
      }
      return v;
    };

    // exception the first time
    EXPECT_THROW({
      auto ptr = map.getOrCreate(5, create);
      }, std::runtime_error);

    // make sure we can still create it correctly the second time
    auto ptr = map.getOrCreate(5, create);
    EXPECT_TRUE(ptr.get() != nullptr);
  }


  {
    SharedLazyMap<int, std::vector<int>> map;
    std::atomic<int> totalCreations{0};

    const int numThreads = 16;
    const int numKeys = 10;
    const int accessesPerThread = 1000;
    const size_t vecSize = 20;
    std::vector<std::thread> threads;

    // Create function that allocates an expensive object
    auto createFunc = [&totalCreations](int key) {
      totalCreations++;
      auto vec = std::make_shared<std::vector<int>>();
      for (auto i = 1u; i <= vecSize; i++) {
        vec->resize(i);
        vec->back() = key;
        vec->front() += vec->size();
      }
      return vec;
    };

    // Launch threads with high contention on limited keys
    for (int t = 0; t < numThreads; t++) {
      threads.emplace_back([&, t]() {
        Rng rng(t);

        for (int i = 0; i < accessesPerThread; i++) {
          int key = rng.rint(numKeys);
          try {
            std::function<std::shared_ptr<std::vector<int>>()> creator = [&]() { return createFunc(key); };
            auto vec = map.getOrCreate(key, creator);
            // Verify the vector contains the expected values
            EXPECT_TRUE(vec.get() != nullptr);
            EXPECT_EQ(vec->size(), vecSize);
            EXPECT_EQ(vec->back(), key);
          }
          catch (std::exception& e) {
            LOG_ERROR("Client caught exception: {}", e.what());
          }
        }
      });
    }

    // Wait for all threads
    for (auto& t : threads) {
      t.join();
    }

    // Verify we created exactly numKeys objects
    EXPECT_EQ(totalCreations.load(), numKeys);
  }
}
