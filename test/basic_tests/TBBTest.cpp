#include <gtest/gtest.h>
#include <stdatomic.h>

#include "solux/util/thread.h"

#include "solux/store/Directory.h"
#include "oneapi/tbb/task_group.h"
#include "oneapi/tbb/flow_graph.h"


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


void foo() {

  // create a graph and a multi-function node
  oneapi::tbb::flow::graph g;
  using MyNodeType = tbb::flow::multifunction_node<Blocker*, std::tuple<Blocker*>>;
  MyNodeType notifier2(g, tbb::flow::unlimited, [](Blocker* blocker, MyNodeType::output_ports_type& op) {
    unused(op);
    blocker->notify();
  });
  MyNodeType notifier(g, tbb::flow::unlimited, [&](Blocker* blocker, MyNodeType::output_ports_type& op) {
    unused(op);
    // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // blocker->notify();
    notifier2.try_put(blocker); // exercise more of the graph machinery
  });

  int n=1000;
  std::vector<std::unique_ptr<Blocker>> blockers;
  for (int i=0; i<n; i++) {
    blockers.emplace_back(std::make_unique<Blocker>([i,&notifier,&blockers]{
      notifier.try_put(blockers[i].get());
    }));
  }

  // create a task group
  oneapi::tbb::task_group tg;
  for (int i=0; i<n; i++) {
    tg.run([i,&blockers]{
      blockers[i]->wait();
    });
  }

  tg.wait();
}

// #define DISABLED_TEST3
#ifdef DISABLED_TEST3
TEST_F(TBBTest, testBlocker) {
  // TODO: why does the blocker strategy still deadlock when used with indexing??? is it the test threads?

  oneapi::tbb::task_arena arena(8);

  arena.execute([&]{
    for (int i=0; i<100; i++) {
      foo();
    }
  });


}
#endif