
#include <gtest/gtest.h>
#include <iostream>
#include <latch>
#include "test/SoluxTest.h"

using namespace solux;

class TaskflowTest : public solux::SoluxTest {
protected:
  tf::Executor& exec = executor(); // copy to local var so no further synchronization

  std::latch doneLatch;
  int topLevelTasks = std::thread::hardware_concurrency() * 2;
  int subtasks = 1000; // number of subtasks to spawn for each top-level task

  std::mutex lock;
  std::unordered_set<int> workers;
  std::atomic_long counter = 0;

  TaskflowTest() : doneLatch(1) {
  }

  void taskA() {
    // std::cout << "worker=" << exec.this_worker_id() << " TaskA" << std::endl;
    // std::this_thread::sleep_for (std::chrono::seconds(1));  // used to make sure all threads are used

    std::lock_guard<std::mutex> guard(lock);
    workers.insert(exec.this_worker_id());
  }

  void taskB() {
    // std::cout << "worker=" << exec.this_worker_id() << " TaskB" << std::endl;

    // OK, now spawn a bunch of different tasks
    for (int i=0; i<subtasks; i++) {
      exec.silent_async([this]{
        taskSub();
      });
    }
  }

  // both tasks in one to try silent_async method
  void taskAB() {
    taskA();
    taskB();
  }

  void taskSub() {
    auto val = counter.fetch_add(1);
    if (val + 1 == subtasks*topLevelTasks) {
      taskDone();
    }
  }

  void taskDone() {
    std::cout << "Done!" << std::endl;
    doneLatch.count_down();
  }

};

// Test multiple task flows all using the same executor and launching more task flows within
// those tasks.
// TODO: test subflows.  Subflows are a way to have a single task hand have that task dynamically spawn
// more tasks as part of itself (these must be completed like normal before the parent continues.)
// An alternative would be to just create the first part of the taskflow and then create a new
// taskflow in the last step of the parent.
TEST_F(TaskflowTest, testMT) {

  // need to keep track of the created taskflow objects or it crashes
  std::vector<std::unique_ptr<tf::Taskflow>> jobs;

  // NOTE: running the same taskflow multiple times doesn't work... (or well, it's defined to run sequentially)
  // Need to create a new taskflow to utilize multiple threads.
  for (int i=0; i<topLevelTasks; i++) {
    tf::Taskflow& taskflow = *jobs.emplace_back(std::make_unique<tf::Taskflow>());

    auto [A, B] = taskflow.emplace(
            [this](){taskA();} ,
            [this](){taskB();}
    );
    A.precede(B);  // A runs before B

    exec.run(taskflow);
  }

  /* alternate method
  for (int i=0; i<topLevelTasks; i++) {
    exec.silent_async([this](){taskAB();});
  }
  */

    // exec.wait_for_all();
  doneLatch.wait();
  std::cout << "Number of workers used for taskA=" << workers.size() << std::endl;

  ASSERT_EQ(counter, topLevelTasks * subtasks);
}
