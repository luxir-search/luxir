
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <solux/index/Inverter.h>
#include <latch>
#include <solux/server/ProtoUpdateMessage.h>

#include "solux/index/IndexWriter.h"
#include "solux/search/IndexReader.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"

#define TEST_DEBUG LOG_TRACE
// #define TEST_DEBUG LOG_DEBUG

using namespace std;
using namespace solux;

class IndexWriterTest : public SoluxTest {
public:
  std::string field = "text_w";

  void addDoc(IndexWriter& iw) {
    auto* inverter = &iw.obtainInverter();
    auto* fieldHandler = &inverter->getIndexHandler(field);
    std::string doc1 = "test";
    inverter->startDoc();
    fieldHandler->index(*inverter, doc1);
    inverter->finishDoc();
    iw.releaseInverter(*inverter);
  }
};


TEST_F(IndexWriterTest, simple) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto* inverter = &iw.obtainInverter();
  auto* fieldHandler = &inverter->getIndexHandler(field);

  std::string doc1 = "now is the time for all good men";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1);
  inverter->finishDoc();

  iw.releaseInverter(*inverter);
  iw.commit();

  IndexReader r1(dir);
  ASSERT_EQ(1, r1.segments().size());
  ASSERT_EQ(1, r1.numDocs());

  inverter = &iw.obtainInverter();
  fieldHandler = &inverter->getIndexHandler(field);

  doc1 = "to come to the aid";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1);
  inverter->finishDoc();
  doc1 = "of their country";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1);
  inverter->finishDoc();

  iw.releaseInverter(*inverter);
  iw.commit();

  IndexReader r2(dir);
  ASSERT_EQ(2, r2.segments().size());
  ASSERT_EQ(3, r2.numDocs());
  ASSERT_GT(r2.commitTime(), r1.commitTime());
}

// Test retrieving IndexReader from the IndexWriter
TEST_F(IndexWriterTest, getReader) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto reader = iw.getIndexReader();
  ASSERT_EQ(0, reader->numDocs());
  ASSERT_EQ(0, reader->segments().size());  // could change depending on impl

  addDoc(iw);

  // no commit, so getIndexReader() should return the same reader
  auto reader2 = iw.getIndexReader();
  ASSERT_EQ(reader, reader2);

  // now make it visible.
  iw.commit();
  reader = iw.getIndexReader();
  ASSERT_EQ(1, reader->numDocs());
  ASSERT_EQ(1, reader->segments().size());

  // now add another doc, commit, but get a reader with permissive freshness
  addDoc(iw);
  iw.commit();
  reader2 = iw.getIndexReader(10000000);  // can be up to 10 seconds old
  ASSERT_EQ(reader, reader2);  // not guaranteed, but should be the case

  // sleep current thread for a microsecond
  std::this_thread::sleep_for(std::chrono::microseconds(1));

  // now test a reader that is not fresh enough
  reader2 = iw.getIndexReader(1);  // can be up to 1 microseconds old! (0 is a special case, so we just chose smallest value we can)
  ASSERT_NE(reader, reader2);  // It's possible this could spuriously fail if the sleep wasn't long enough or the system clock is changed.
  ASSERT_EQ(2, reader2->numDocs());
  ASSERT_EQ(2, reader2->segments().size());
}


// Test automatic merging kick-off
TEST_F(IndexWriterTest, autoMerge) {
  // This scope guard no longer needed since SoluxTest clears all listeners.
  // auto cleaner = solux::scope_guard([](){ solux::Signal::unlisten("mergeStart");});

  auto iterations = 1;  // increase for more thorough testing
  for (auto iter=0; iter<iterations; iter++) {
    RAMDir dir;
    IndexWriter iw(dir);
    int MERGE_FACTOR = 3;
    iw.mergePolicy->setMergeFactor(MERGE_FACTOR);
    iw.mergePolicy->refresh();  // should be a no-op at this point since no existing segs.

    // add  segments
    for (int i = 0; i < MERGE_FACTOR - 1; i++) {
      addDoc(iw);
      iw.commit();
    }

    // make sure we're making the segments we think we are:
    auto reader = iw.getIndexReader();
    ASSERT_EQ(MERGE_FACTOR - 1, reader->segments().size());
    ASSERT_EQ(MERGE_FACTOR - 1, reader->numDocs());

    addDoc(iw);

    // MERGE_FACTOR segments of the same size will set off an auto-merge.
    // For this reason, we need a callback to try grabbing the new IndexReader before the merge completes.
    // We need to block the merge operation until we've grabbed a new reader.
    std::latch mergeStart(1);
    auto callback = [&mergeStart](void* a, void* b, void* c) -> void* {
      unused(a,b,c);
      mergeStart.wait();
      return nullptr;
    };

    solux::Signal::listen("mergeStart", std::move(callback));

    // this should cause a segment flush and a merge to kick off, but we've blocked the merge from completing until
    // later.
    // NOTE: this deadlocked since the wait() in the commit call work-steals the mergeSegments task (which will
    // only be un-blocked after the commit call completes).
    // Seems like we can't have both the commit work and the merge work be on the same thread.

    /* deadlock version
    iw.commit();
    // pre-merge view
    reader = iw.getIndexReader();
    ASSERT_EQ(reader->numDocs(), MERGE_FACTOR);
    ASSERT_EQ(reader->segments().size(), MERGE_FACTOR);
    mergeStart.count_down(); // let the merge continue
    */

    // What about using an async callback?
    // What if I force it to be called from a separate thread?

    // finish commit callback
    auto finishCommit = [&]() {
      auto reader = iw.getIndexReader();
      mergeStart.count_down();  // let merge continue
      EXPECT_EQ(reader->numDocs(), MERGE_FACTOR);
      EXPECT_EQ(reader->segments().size(), MERGE_FACTOR);
    };

    iw.commit(std::move(finishCommit), UpdateMessage::CommitType::COMMIT);
    /* not needed to avoid deadlock... using a callback and not a blocker that could work-steal was enough.
    arena.execute([&](){
      iw.commit(UpdateMessage::CommitType::COMMIT, false, std::move(finishCommit));
    });
    */

    // Now wait until the merge completes.
    // If the commit is run in the same thread, this call can work-steal the mergeSegments task and deadlock.
    iw.updateGraph.wait_for_all();

    reader = iw.getIndexReader();
    ASSERT_EQ(reader->numDocs(), MERGE_FACTOR);
    ASSERT_EQ(reader->segments().size(), 1);
  }
}


// Multithreaded test of IndexWriter updates, commits, merges, and IndexReader reopen during those.
TEST_F(IndexWriterTest, multiThreaded) {
  int requestThreads = 4;
  int docsToAdd = 100;
  int percentReads = 20;
  int percentCommits = 50;  // really stress segment flushing / merging

  RAMDir dir;
  IndexWriter iw(dir);
  int MERGE_FACTOR = 3;
  iw.mergePolicy->setMergeFactor(MERGE_FACTOR);
  iw.mergePolicy->refresh();  // should be a no-op at this point since no existing segs.

  std::atomic_long docsRequested(0);
  std::atomic_long docsAdded(0);
  std::atomic_long docsVisible(0);
  std::atomic_long commitsRequested(0);
  std::atomic_long commits(0);

  class UpdateInfo {
  public:
    int32_t seqNum;
    short numAdds;
    byte commitType;
  };
  std::vector<UpdateInfo> updates;
  bool recordUpdates = false;
  if (recordUpdates) {
    updates.reserve(docsToAdd * 2);
  }
  std::mutex testMutex;


  class TestProtoUpdateMessage : public ProtoUpdateMessage {
  public:
    solux::proto::UpdateRequest updateRequest;
    std::function<void(TestProtoUpdateMessage&)> callback = nullptr;

    // note - we are passing a not-yet-constructed UpdateRequest to the base class constructor... this isn't generally
    // safe so if is test fails, we need to fix this (a holder class for the request and the ProtoUpdateMessage?
    TestProtoUpdateMessage(std::function<void(TestProtoUpdateMessage&)> callback) : ProtoUpdateMessage(&updateRequest), callback(callback) {}

    void done(IndexWriter& iw) override {
      unused(iw);
      if (callback) {
        callback(*this);
      }
      delete this;
    }
  };

  auto cb = [&](TestProtoUpdateMessage& msg) {
            TEST_DEBUG("done called! adds in this request={}", updateRequest.docs_size());
    if (msg.updateRequest.commit() == solux::proto::UpdateRequest::COMMIT) {
      commits++;
    }
    docsAdded += msg.updateRequest.docs_size();
    if (recordUpdates) {
      std::lock_guard<std::mutex> lock(testMutex);
      for (int i = 0; i < msg.updateRequest.docs_size(); i++) {
        UpdateInfo ui;
        ui.seqNum = msg.updateVersion;
        ui.numAdds = msg.updateRequest.docs_size();
        ui.commitType = (byte)msg.commit;
        updates.push_back(ui);
      }
    }

#ifdef REMOVED
    // this isn't valid test code since done() messages *can* be called out of order.  Updates with a commit
    // have to go through further through the pipeline and can thus have their done() method called later.
    auto localLastUpdateSeen = lastUpdateSeen.load();
    if ((int64_t)msg.seqNum < localLastUpdateSeen) {
      LOG_ERROR("Out of order update done()! {} vs {}", msg.seqNum, localLastUpdateSeen);
      FAIL();
    }
    if ((int64_t)msg.seqNum != localLastUpdateSeen + 1) {
      LOG_ERROR("Update done() skipped a sequence number! this={} last={}", msg.seqNum, localLastUpdateSeen);
      FAIL();
    }
    while ((int64_t)msg.seqNum > localLastUpdateSeen) {
      lastUpdateSeen.compare_exchange_weak(localLastUpdateSeen, (int64_t)msg.seqNum);
      localLastUpdateSeen = lastUpdateSeen.load();
    }
#endif

  };



  try {
    tbb::task_group tasks;

    for (int iter = 0; iter < requestThreads; iter++) {
      tasks.run([&]() {
                  try {
                    bool writesDone = false;
                    for (;;) {
                      if (writesDone || rng.rint(100) < percentReads) {
                        // do this *before* opening the reader, so we can ensure that the reader should see at least
                        // that many updates.
                        auto globalDocsVisible = docsVisible.load();

                        auto reader = iw.getIndexReader();
                        auto localDocsVisible = reader->numDocs();
                        EXPECT_GE(localDocsVisible, globalDocsVisible);
                        while (localDocsVisible > globalDocsVisible) {
                          if (!docsVisible.compare_exchange_weak(globalDocsVisible, localDocsVisible)) {
                            globalDocsVisible = docsVisible.load();
                          }
                        }

                        // LOG_DEBUG("\tvisible docs: {} segs: {}", localDocsVisible, reader->segments().size());

                        if (globalDocsVisible >= docsToAdd && globalDocsVisible >= docsAdded) {
                          // we've seen all the docs
                          break;
                        }
                      }

                      // NOTE: since write submission is async and done in a loop, this can pile up a lot of writes in the queue
                      // really fast!  Perhaps we should yield when docsRequested - docsAdded is too large?
                      if (!writesDone) {
                        auto* msg = new TestProtoUpdateMessage(cb);
                        solux::proto::UpdateRequest* ureq = &msg->updateRequest;
                        bool doCommit = rng.rint(100) < percentCommits;
                        int64_t numAdds = rng.rint(doCommit ? 0 : 1,
                                                   3);  // lower bound on number of adds is 0 if we're going to commit

                        // make sure we don't go over the number of docs we want to add
                        // this makes it harder to figure out when we should do final commits.
                        if (numAdds > 0) {
                          for (;;) {
                            auto localDocsRequested = docsRequested.load();
                            numAdds = std::min(numAdds, docsToAdd - localDocsRequested);
                            if (numAdds == 0) {
                              doCommit = true;  // turn into a commit if it wasn't already.
                            }
                            assert(localDocsRequested + numAdds <= docsToAdd);  // sanity check
                            if (docsRequested.compare_exchange_weak(localDocsRequested, localDocsRequested + numAdds)) {
                              break;
                            }
                          }
                        }

                        ureq->set_commit(doCommit ? solux::proto::UpdateRequest::COMMIT : solux::proto::UpdateRequest::NO_COMMIT);
                        // Normal ProtoUpdateMessage sets commit from request in constructor. Since that has already passed, need to do it manually here.
                        msg->commit = doCommit ? UpdateMessage::CommitType::COMMIT : UpdateMessage::CommitType::NO_COMMIT;
                        for (int i = 0; i < numAdds; i++) {
                          auto& fields = *ureq->add_docs()->mutable_fields();
                          fields[field].set_s("now is the time for all");
                        }

                        if (doCommit) {
                          commitsRequested++;
                        }

                        if (doCommit && docsRequested.load() >= docsToAdd) {
                          // we're done with writes as long as we ended with a commit.
                          writesDone = true;
                        }

                        TEST_DEBUG("\tsubmitting update with {} adds, commit={}", numAdds, doCommit);
                        auto success = iw.submitUpdate(msg);
                        if (!success) {
                          FAIL();
                        }
                      }

                      TEST_DEBUG("\t\tdocsRequested: {}, docsAdded: {}, docsVisible: {}, commitsRequested: {}, commits: {}",
                              docsRequested.load(), docsAdded.load(), docsVisible.load(), commitsRequested.load(),
                              commits.load());
                    }
                    TEST_DEBUG("Done with request thread");

                  } catch (std::exception& e) {
                    LOG_ERROR("################# Exception in request thread: {}", e.what());
                    FAIL();
                  }
                }

      );
    }

    tasks.wait();
    iw.updateGraph.wait_for_all();

    // If request threads are failing to stop, but this block of code above tasks.wait() and uncomment the sleep
    // std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    while (docsRequested.load() < docsToAdd || docsVisible.load() < docsToAdd) {
      auto reader = iw.getIndexReader();
      LOG_INFO("### Main Thread docsRequested: {}, docsAdded: {}, docsVisible: {}, commitsRequested: {}, commits: {}",
              docsRequested.load(), docsAdded.load(), reader->numDocs(), commitsRequested.load(), commits.load());


      if (docsRequested.load() >= docsToAdd && reader->numDocs() < docsToAdd) {
        if (iw.lastAdvertisedCommitTime != reader->commitTime()) {
          LOG_ERROR("Reader not seeing last advertised commit time! {} vs {}", iw.lastAdvertisedCommitTime.load(), reader->commitTime());
        }

        iw.debugInfo();

        // let's look at the last number of updates:
        if (!recordUpdates) {
          LOG_INFO("Test: consider re-running with recordUpdates=true in this test to see the last updates");
        }
        int start = std::max(0, (int)updates.size() - 100);
        for (int i = start; i < (int)updates.size(); i++) {
          auto& ui = updates[i];
          LOG_INFO("{}: seqNum={} numAdds={} commitType={}", i, ui.seqNum, ui.numAdds, ui.commitType);
        }

        iw.commit();
        reader = iw.getIndexReader();
        if (reader->numDocs() == docsToAdd) {
          LOG_ERROR("FINAL COMMIT MADE DOCS VISIBLE! Test Bug or IW bug?");
          FAIL();
        } else {
          break;
        }
      }
    }


  } catch (std::exception& e) {
    LOG_ERROR("################# Exception in main thread: {}", e.what());
    FAIL();
  }

  EXPECT_EQ(docsRequested.load(), docsToAdd);
  EXPECT_EQ(docsRequested.load(), docsAdded.load());
  EXPECT_EQ(docsRequested.load(), docsVisible.load());
  EXPECT_EQ(commitsRequested.load(), commits.load());
}


// Test that _version_ field is present when overwrite=true and absent otherwise
TEST_F(IndexWriterTest, versionFieldOverwrite) {
  using namespace solux::test;
  
  CollectionHelper helper("main");
  helper.clear();

  Doc doc1 = flatdoc("id", "doc1", "text_w", "hello world");
  helper.index(doc1, UpdateMessage::COMMIT, false);

  Doc doc2 = flatdoc("id", "doc2", "text_w", "hello version world");
  auto result2 = helper.index(doc2, UpdateMessage::COMMIT, true);

  Doc doc3 = flatdoc("id", "doc3", "text_w", "hello third world");
  auto result3 = helper.index(doc3, UpdateMessage::COMMIT, true);
  
  EXPECT_GT(result3.updateVersion, result2.updateVersion);

  auto* req = LocalReq::create(helper.getSearchEngine());
  auto docs = req->collection("main")
                 .allQuery()
                 .fields({"id", "text_w", "_version_"})
                 .limit(-1)
                 .execute()
                 .getDocs();


  req->done();
  ASSERT_EQ(3, docs.size());
  
  Doc expectedDoc1 = flatdoc("id", "doc1", "_version_", std::numeric_limits<int64_t>::min());
  Doc expectedDoc2 = flatdoc("id", "doc2", "_version_", (int64_t)(result2.updateVersion));
  Doc expectedDoc3 = flatdoc("id", "doc3", "_version_", (int64_t)(result3.updateVersion));
  
  bool foundDoc1 = containsDoc(docs, expectedDoc1);
  EXPECT_TRUE(foundDoc1);

  bool foundDoc2 = containsDoc(docs, expectedDoc2);
  EXPECT_TRUE(foundDoc2);

  bool foundDoc3 = containsDoc(docs, expectedDoc3);
  EXPECT_TRUE(foundDoc3);
}
