#include <gtest/gtest.h>

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
  };


  class IW {
  public:
    // for testing only, not necessary for strategy
    int expectedCommitNum = 0;
    int totalCommits = 0;
    std::atomic_int flushSegCount = 0;
    std::set<std::thread::id> seenThreads;

    // index lock
    std::mutex indexLock;

    // flow graph to handle commits
    std::unique_ptr<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> > commitMain;
    std::unique_ptr<tbb::flow::sequencer_node<UpdateMessage*> > sequencer;
    std::unique_ptr<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> > commitFinishNode;
    tbb::flow::graph commitGraph;

    IW() {
      commitMain = std::make_unique<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> >(commitGraph, tbb::flow::unlimited,
       [this](UpdateMessage* msg) -> UpdateMessage* {
        this->commitBody(*msg);
        return msg;
      });
      sequencer = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*> >(commitGraph, [](UpdateMessage* msg) ->std::size_t { return msg->commitNum; });

      // make the commitFinishNode single-threaded so that commits can't get reordered.
      commitFinishNode = std::make_unique<tbb::flow::function_node<UpdateMessage*, UpdateMessage*> >(commitGraph, 1,
       [this](UpdateMessage* msg) -> UpdateMessage* {
        // write the index info
        this->finish(*msg);
        return msg;
      });

      tbb::flow::make_edge(*commitMain, *sequencer);
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
      tbb::task_group taskGroup;
      int numSegs = 10;
      // in the real IW, this won't be so easy because some of the segments that need to be flushed may be in use.
      for (int i = 0; i < numSegs; i++) {
        taskGroup.run([this, &umsg] { this->flushSeg(umsg); });
      }
      taskGroup.wait();
    }

    void flushSeg(UpdateMessage& umsg) {
      LOG_INFO("  flush for commit {}", umsg.commitNum);

      flushSegCount++;
      // sleep for a millisecond to simulate flushing a segment
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::unique_lock<std::mutex> lock(indexLock);
      seenThreads.insert(std::this_thread::get_id());
    }

    void finish(UpdateMessage& msg) {
      LOG_INFO("  finish for commit {}", msg.commitNum);
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
    oneapi::tbb::task_arena ta(16);
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

#ifdef DISABLED_TEST
TEST_F(TBBTest, TBBCommitStrat) {
  testCommitStrat(1000);
}
#endif