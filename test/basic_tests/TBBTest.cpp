#include <gtest/gtest.h>
#include <stdatomic.h>
#include <oneapi/tbb/parallel_invoke.h>
#include <oneapi/tbb/parallel_for.h>

#include "solux/util/thread.h"

#include "solux/store/Directory.h"
#include "oneapi/tbb/task_group.h"
#include "oneapi/tbb/flow_graph.h"
#include "oneapi/tbb/concurrent_queue.h"

using namespace solux;

// This is to test the TBB strategy for enforcing that commits are finished in order.
class TBBTest : public testing::Test {
protected:


  class UpdateMessage {
  public:
    int64_t commitNum = 0;
    std::atomic_int leftToFlush = 0; // number of segments left to flush
  };


  class IW {
  public:
    oneapi::tbb::task_arena taskArena;

    // for testing only, not necessary for strategy
    int expectedCommitNum = 0;
    int totalCommits = 0;
    std::atomic_int flushSegCount = 0;
    std::set<std::thread::id> seenThreads;

    // index lock
    std::mutex indexLock;

    using SegFlushNodeType = tbb::flow::multifunction_node<UpdateMessage*, std::tuple<UpdateMessage*>>;

    // flow graph to handle commits
    std::unique_ptr<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> > commitMain;
    std::unique_ptr<SegFlushNodeType > segFlushNode; // called when a segment is flushed
    std::unique_ptr<tbb::flow::sequencer_node<UpdateMessage*> > sequencer;
    std::unique_ptr<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> > commitFinishNode;  // TODO: error here... nothing to read from output port?
    tbb::flow::graph commitGraph;

    IW() {
      commitMain = std::make_unique<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> >(commitGraph, tbb::flow::unlimited,
       [this](UpdateMessage* msg) -> UpdateMessage* {
        this->commitBody(*msg);
        return msg;
      });

      segFlushNode = std::make_unique<SegFlushNodeType>(commitGraph, tbb::flow::unlimited,
       [this](UpdateMessage* msg, SegFlushNodeType::output_ports_type& op) {
         this->flushSeg(*msg);
         int left = --msg->leftToFlush;
         if (left <= 0) {
           // no more flushes left to wait for, so put message to output port.
           std::get<0>(op).try_put(msg);
         }
      });

      sequencer = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*> >(commitGraph, [](UpdateMessage* msg) ->std::size_t { return msg->commitNum; });

      // make the commitFinishNode single-threaded so that commits can't get reordered.
      commitFinishNode = std::make_unique<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> >(commitGraph, 1,
       [this](UpdateMessage* msg) -> UpdateMessage* {
        // write the index info
        this->finish(*msg);
        return msg;
      });


      // tbb::flow::make_edge(*commitMain, *segFlushNode);
      tbb::flow::make_edge(*segFlushNode, *sequencer);
      tbb::flow::make_edge(*sequencer, *commitFinishNode);
    }

    // Perhaps return CommitInfo here in case stats or other stuff should be relayed back to the caller?
    void commit() {
      std::unique_lock<std::mutex> lock(indexLock);
      seenThreads.insert(std::this_thread::get_id());
      // if there are no *new* changes, we still want to wait for the previous commit to finish.
      UpdateMessage* umsg = new UpdateMessage();  // this stack reference is what will keep the CommitInfo alive during the commit
      umsg->commitNum = totalCommits;
      totalCommits++;
      lock.unlock();

      auto success = commitMain->try_put(umsg);
      assert(success);
    }


    void commitBody(UpdateMessage& umsg) {
      int numSegs = 10;
      umsg.leftToFlush = numSegs;

      for (int i=0; i<numSegs; i++) {
        // do async so things can get mixed up a bit between messages
        taskArena.execute([this, &umsg] {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          this->segFlushNode->try_put(&umsg);
        });
        // segFlushNode->try_put(&umsg);
      }
    }

#ifdef REMOVED
    void commitBody(UpdateMessage& umsg) {
      tbb::task_group taskGroup;
      int numSegs = 10;
      // in the real IW, this won't be so easy because some of the segments that need to be flushed may be in use.
      // option1: create a sub-graph with an async node for each segment that is outstanding (that can be called after
      // a flush has completed).
      // option 2: create a multi-function node that can be called when a segment has finished flushing.  When all
      // needed segments have finished flushing, send the update message to the next node.  This can be a simple count
      // to tell if we need to wait for more segments.
      // TODO: still need to figure out how to work in segment merges.
      //   if a commit is pending, perhaps wait until that commit finishes before looking at what segments we can merge.
      //   This could optimize then case when we have 13 small segments coming but we decide to merge only 10 of them?
      for (int i = 0; i < numSegs; i++) {
        taskGroup.run([this, &umsg] { this->flushSeg(umsg); });
      }
      taskGroup.wait();
    }
#endif

    void flushSeg(UpdateMessage& umsg) {
      LOG_INFO("  flush for commit {} flushLeft={}", umsg.commitNum, umsg.leftToFlush.load());

      flushSegCount++;
      // sleep for a millisecond to simulate flushing a segment
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::unique_lock<std::mutex> lock(indexLock);
      seenThreads.insert(std::this_thread::get_id());
    }

    void finish(UpdateMessage& msg) {
      LOG_INFO("  finish for commit {}", msg.commitNum);
      EXPECT_EQ(0, msg.leftToFlush);
      // check that the commits finish in order
      std::unique_lock<std::mutex> lock(indexLock);
      EXPECT_EQ(msg.commitNum, expectedCommitNum);
      expectedCommitNum += 1;
      delete &msg;
    }


  };


  void testCommitStrat(int numTasks) {
    IW iw;

    // now launch a certain number of commit tasks in parallel
    oneapi::tbb::task_group tg;
    for (int i = 0; i < numTasks; i++) {
      tg.run([&iw] { iw.commit(); });
    }

    tg.wait();

    // sleep for a few seconds to let the commits finish... it's not easy to have a task sitting around calling wait_for_all()
    // on the graph, so this is a hack to make sure that messages will still be processed even without that.
    std::this_thread::sleep_for(std::chrono::seconds(3));

    LOG_INFO("seenThreads={} flushSegCount={}", iw.seenThreads.size(), iw.flushSegCount.load());

    ASSERT_EQ(iw.expectedCommitNum, numTasks);

    // TBB docs say we must call this, but we really prefer not.
    // iw.commitGraph.wait_for_all();
  }
};

// #define DISABLED_TEST
#ifdef DISABLED_TEST
TEST_F(TBBTest, TBBCommitStrat) {
  testCommitStrat(1000);
}
#endif


// #define DISABLED_TEST2
#ifdef DISABLED_TEST2
// RESULT: Yes, if we send to a node and it has no one to read from is output port, buffering is done and we eventually run out of memory!
// If this happens in our production code, how can we test for it?
// Adding the rejecting policy to the function_node did not help for some reason... presumably because tbb::flow::unlimited?
// It's unclear when we are just seeing creation of tasks running ahead of the graph as well.
// Testing with a multifunction node *and* with a Msg that is the size of the pointer, I see no growth.
//   Perhaps TBB has some code that handles things differently for large messages?
TEST_F(TBBTest, testNoConsumer) {
  struct Msg {
    // char data[1024];  // something big so we can see any buffering
    char data[8];  // something the size of a pointer
  };

  struct Empty{};

  using MyNodeType = tbb::flow::multifunction_node<Msg, std::tuple<Msg>>;

  std::atomic_int count = 0;


  {
    // std::unique_ptr<tbb::flow::function_node<Msg, Msg, tbb::flow::rejecting> > fnode;
    std::unique_ptr<tbb::flow::function_node<Msg, Empty> > fnode2;
    std::unique_ptr<MyNodeType> fnode;
    tbb::flow::graph commitGraph;

    fnode = std::make_unique<MyNodeType>(commitGraph, 32,
                                         [&](Msg msg, MyNodeType::output_ports_type& op) {
                                           // increment count
                                            count++;
                                         });


    // fnode = std::make_unique<tbb::flow::function_node<Msg, Msg, tbb::flow::rejecting> >(commitGraph, tbb::flow::unlimited,
    // fnode = std::make_unique<tbb::flow::function_node<Msg, Msg, tbb::flow::rejecting> >(commitGraph, 1,
    fnode2 = std::make_unique<tbb::flow::function_node<Msg, Empty> >(commitGraph, tbb::flow::unlimited,
                                                                  [&](Msg msg) -> Empty {
                                                                    count++;
                                                                    return {};
                                                                  });

    int numTasks = 100000000;
    oneapi::tbb::task_group tg;
    for (int i = 0; i < numTasks; i++) {
      // tg.run( [&]{
                bool success = fnode->try_put(Msg());
                // bool success = fnode2->try_put(Msg());
                if (false && i % 1000000 == 0) {
                  // sleep for a bit to give time for tasks to execute
                  std::cout << "i=" << i << " count=" << count << std::endl;
                  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
                ASSERT_TRUE(success);
      //        }
      // );
    }

    tg.wait();
  }

}
#endif


void doBlockers(int n) {

  class MyBlocker : public Blocker {
  public:
    std::atomic_int status = 0;
    void wait() {
      // LOG_DEBUG("BLOCKING this={}", (void*)this);
      Blocker::wait();
      // LOG_DEBUG("UNBLOCK  this={}", (void*)this);
    }

    void notify() {
      // LOG_DEBUG("NOTIFY   this={} status=", (void*)this, status.load());
      Blocker::notify();
    }
  };

  // create a graph and a multi-function node
  oneapi::tbb::flow::graph g;
  using MyNodeType = tbb::flow::multifunction_node<MyBlocker*, std::tuple<Blocker*>>;
  MyNodeType notifier2(g, tbb::flow::unlimited, [](MyBlocker* blocker, MyNodeType::output_ports_type& op) {
    unused(op);
    blocker->status = 1;
    blocker->notify();
  });
  MyNodeType notifier(g, tbb::flow::unlimited, [&](MyBlocker* blocker, MyNodeType::output_ports_type& op) {
    unused(op);
    // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // blocker->notify();
    notifier2.try_put(blocker); // exercise more of the graph machinery
  });

  std::vector<std::unique_ptr<MyBlocker>> blockers;
  for (int i=0; i<n; i++) {
    blockers.emplace_back(std::make_unique<MyBlocker>());
    // notifier.try_put(blockers.back().get());
  }

  // create a task group
  oneapi::tbb::task_group tg;
  for (int i=0; i<n; i++) {
    tg.run([&,i]{
      blockers[i]->wait();
      EXPECT_EQ(1, blockers[i]->status.load());  // if wait returned, notify must have been called.
    });
    tg.run([&,i]{
      // cause some blockers to call wait() first, others call notify() first.
      notifier.try_put(blockers[(i+n/2)%n].get());
    });
  }

  tg.wait();
  g.wait_for_all();
}

TEST_F(TBBTest, testBlocker) {
  doBlockers(100);
}

//#define DISABLED_TEST3
#ifdef DISABLED_TEST3
TEST_F(TBBTest, testBlockerStress) {
  oneapi::tbb::task_arena arena(8);
  arena.execute([&]{
    for (int i=0; i<100; i++) {
      doBlockers(1000);
    }
  });
}
#endif

#ifdef REMOVED
/*
// RESULTS when sleeping in main thread before calling wait() or wait_all():
//   try-put from main thread:
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045366073 processTime=1713571591045410079 diff=44006
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045388079 processTime=1713571591045418648 diff=30569
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045388249 processTime=1713571591045424201 diff=35952
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045388919 processTime=1713571591045426676 diff=37757
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045389029 processTime=1713571591045426981 diff=37952
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045389149 processTime=1713571591045431380 diff=42231
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045389289 processTime=1713571591045431450 diff=42161
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045389409 processTime=1713571591045432015 diff=42606
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045389549 processTime=1713571591045434670 diff=45121
[2024-04-19 20:06:32.045] [info] [TBBTest.cpp:428] submitTime=1713571591045389659 processTime=1713571591045435039 diff=45380
//   try-put from task in task-group:
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978350597 processTime=1713571702978356426 diff=5829
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355127 processTime=1713571702978356406 diff=1279
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355306 processTime=1713571702978356376 diff=1070
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355426 processTime=1713571702978356356 diff=930
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355536 processTime=1713571702978356326 diff=790
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355576 processTime=1713571702978356306 diff=730
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355696 processTime=1713571702978356276 diff=580
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355726 processTime=1713571702978356246 diff=520
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355766 processTime=1713571702978356216 diff=450
[2024-04-19 20:08:23.978] [info] [TBBTest.cpp:440] submitTime=1713571702978355806 processTime=1713571702978356176 diff=370
//   task_group.run() from main thread:
 [2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466022433 processTime=1713572002466063917 diff=41484
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046119 processTime=1713572002466070906 diff=24787
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046319 processTime=1713572002466070986 diff=24667
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046549 processTime=1713572002466073666 diff=27117
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046659 processTime=1713572002466074766 diff=28107
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046759 processTime=1713572002466078005 diff=31246
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046859 processTime=1713572002466078435 diff=31576
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046889 processTime=1713572002466085244 diff=38355
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046959 processTime=1713572002466085654 diff=38695
[2024-04-19 20:13:23.466] [info] [TBBTest.cpp:451] submitTime=1713572002466046989 processTime=1713572002466086754 diff=39765
//   task_group.run() from task in that task-group:
[2024-04-19 20:21:01.083] [info] [TBBTest.cpp:472] Time until first submit=40409
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251829578 processTime=1713572121251835387 diff=5809
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834397 processTime=1713572121251835357 diff=960
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834487 processTime=1713572121251835337 diff=850
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834577 processTime=1713572121251835307 diff=730
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834657 processTime=1713572121251835287 diff=630
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834717 processTime=1713572121251835257 diff=540
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834797 processTime=1713572121251835237 diff=440
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834827 processTime=1713572121251835207 diff=380
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834857 processTime=1713572121251835177 diff=320
[2024-04-19 20:15:22.251] [info] [TBBTest.cpp:463] submitTime=1713572121251834887 processTime=1713572121251835147 diff=260

 TODO: try this on native linux (the above numbers are from WSL Ubuntu 22.04)
*/

//
// try_put to a multifunction_node in a graph takes about ~50ns for the first task to be scheduled
// on a different thread.
TEST_F(TBBTest, testSchedulingLatency) {


  // test the time it takes for another thread to work-steal a task from a blocked thread.
  class Msg {
  public:
    Msg() = default;
    Msg(const Msg&) = delete;
    Msg& operator=(const Msg&) = delete;

    std::chrono::time_point<std::chrono::high_resolution_clock> submitTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> processTime;
    std::thread::id threadId;

    void start() {
      submitTime = std::chrono::high_resolution_clock::now();
    }

    void submitted() {
      processTime = std::chrono::high_resolution_clock::now();
      threadId = std::this_thread::get_id();
    }
  };

  int n = 10;
  std::vector<Msg> messages(n);

  // create a graph and a multi-function node
  oneapi::tbb::flow::graph g;
  using MyNodeType = tbb::flow::multifunction_node<Msg*, std::tuple<Msg*>>;
  MyNodeType processor(g, tbb::flow::unlimited, [](Msg* msg, MyNodeType::output_ports_type& op) {
    unused(op);
    msg->submitted();
  });

  oneapi::tbb::task_group tg;
  oneapi::tbb::task_arena arena;


  // lets try warming up the arena / task group.  Nope... didn't help.

  arena.execute([&]{
  int res=0;
  for (int i=0; i<10; i++) {
    tg.run([&]{res++;});
  }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(5));


  auto start = std::chrono::high_resolution_clock::now();

  // what if there is a *different* thread that has called wait?  No, didn't help.
  /*
  tg.run([&]{
    for (;;) {
      tg.wait();
    }
  });
   */

  // submit requests from a different thread
  /*
  std::thread t([&]{
    for (auto& msg : messages) {
      auto* mptr = &msg;
      mptr->start();
      tg.run([mptr]{
        mptr->submitted();
      });
    }
  });
   */

/*
  // comment out first and last line to see difference between running in a task and running in the main thread.
  // arena.enqueue([&]{
  tg.run([&]{
    for (auto& msg : messages) {
      auto* mptr = &msg;
      mptr->start();
      tg.run([mptr]{
        mptr->submitted();
      });
    }
  });
  */

/*
  tg.run([&] {
    // try oneapi::tbb::parallel_for (~36usec latency, all on *this* thread though)
    for (auto& msg: messages) {
      msg.start();
    }
    oneapi::tbb::parallel_for(0, n, [&](int i) {
      messages[i].submitted();
    });
  });
*/

/*
  // arena.execute([&]{
//  arena.enqueue([&]{
  for (auto& msg : messages) {
    auto* mptr = &msg;
    mptr->start();
    arena.enqueue([mptr]{
    // arena.execute([mptr]{
    // tg.run([mptr]{
        mptr->submitted();
    });
  }
//  });
*/


  // comment out first and last line to see difference between running in a task and running in the main thread.
//  tg.run([&]{
    for (int i=0; i<n; i++) {
      messages[i].start();
      processor.try_put(&messages[i]);
    }
//  });



/*
  // lets try a TBB concurrent queue (a little  improvement, but not much)
  oneapi::tbb::concurrent_queue<Msg*> queue;
  Blocker blocker;
  bool done = false;
  tg.run([&]{
    while (!done) {
      // blocker.wait();  // if commented out, just spin
      Msg* msg;
      if (queue.try_pop(msg)) {
        tg.run([msg] {
          msg->submitted();
        });
      }
      // blocker.reserve_wait();
    }
  });


  for (auto& msg : messages) {
    msg.start();
    queue.push(&msg);
    // blocker.notify();
  }
*/


  // sleep for a bit to give time for tasks to execute *not* in this thread.
  // comment this out to see the difference of scheduling on *this* thread.
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  g.wait_for_all();
 // tg.wait();


  // print out the time between our start and when the first task was submitted
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(messages[0].submitTime - start).count();
  std::cout << "Time until first submit=" << elapsed << " this thread is " << std::this_thread::get_id() << std::endl;

  for (auto &msg : messages) {
    // print out the duration in nanoseconds and what thread it was processed on.
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(msg.processTime - msg.submitTime).count();
    // LOG_INFO("latency={} threadId={}", msg.submitTime.time_since_epoch().count(), ns);
    std::cout << "latency=" << ns << " threadId=" << msg.threadId << std::endl;
  }


}
#endif
