
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <filesystem>
#include <thread>
#include <random>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <memory>
#include <solux/index/Inverter.h>
#include <latch>
#include <solux/server/ProtoUpdateMessage.h>

#include "solux/index/IndexWriter.h"
#include "solux/search/IndexReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/util/Signal.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

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
  ASSERT_EQ(1, r1.maxDoc());

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
  ASSERT_EQ(3, r2.maxDoc());
  ASSERT_GT(r2.commitTime(), r1.commitTime());
}

// Test retrieving IndexReader from the IndexWriter
TEST_F(IndexWriterTest, getReader) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto reader = iw.getIndexReader();
  ASSERT_EQ(0, reader->maxDoc());
  ASSERT_EQ(0, reader->segments().size());  // could change depending on impl

  addDoc(iw);

  // no commit, so getIndexReader() should return the same reader
  auto reader2 = iw.getIndexReader();
  ASSERT_EQ(reader, reader2);

  // now make it visible.
  iw.commit();
  reader = iw.getIndexReader();
  ASSERT_EQ(1, reader->maxDoc());
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
  ASSERT_EQ(2, reader2->maxDoc());
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
    ASSERT_EQ(MERGE_FACTOR - 1, reader->maxDoc());

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

    // NOTE: code below deadlocked since the wait() in the commit call work-steals the mergeSegments task (which will
    // only be un-blocked after the commit call completes).
    // Seems like we can't have both the commit work and the merge work be on the same thread.

    /* deadlock version
    iw.commit();
    // pre-merge view
    reader = iw.getIndexReader();
    ASSERT_EQ(reader->maxDoc(), MERGE_FACTOR);
    ASSERT_EQ(reader->segments().size(), MERGE_FACTOR);
    mergeStart.count_down(); // let the merge continue
    */

    // What about using an async callback?
    // What if I force it to be called from a separate thread?

    // finish commit callback
    auto finishCommit = [&]() {
      auto reader = iw.getIndexReader();  // refresh the reader to see the new doc, but before the merge completes.
      mergeStart.count_down();  // let merge continue
      EXPECT_EQ(reader->maxDoc(), MERGE_FACTOR);
      EXPECT_EQ(reader->segments().size(), MERGE_FACTOR);
    };

    // this commit will cause the segment to flush and the merge to kick off
    iw.commit(std::move(finishCommit), UpdateMessage::CommitType::COMMIT);
    /* not needed to avoid deadlock... using a callback and not a blocker that could work-steal was enough.
    arena.execute([&](){
      iw.commit(UpdateMessage::CommitType::COMMIT, false, std::move(finishCommit));
    });
    */

    // Now wait until the merge completes.
    // If the commit is run in the same thread, this call can work-steal the mergeSegments task and deadlock.
    iw.updateGraph.wait_for_all();

    // depending on if the merger does a commit on its own, we may not see the merged segment
    // yet.  Currently, if the merger detects no indexing activity, it will request a new commit.

    reader = iw.getIndexReader();
    ASSERT_EQ(reader->maxDoc(), MERGE_FACTOR);
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
            TEST_DEBUG("done called! adds in this request={}", msg.updateRequest.docs_size());
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
    // change to using standard threads
    std::vector<std::thread> threads;

    for (int iter = 0; iter < requestThreads; iter++) {
      // create a thread
      threads.emplace_back([&]() {
                  try {
                    bool writesDone = false;
                    for (;;) {
                      if (writesDone || rng.rint(100) < percentReads) {
                        // do this *before* opening the reader, so we can ensure that the reader should see at least
                        // that many updates.
                        auto globalDocsVisible = docsVisible.load();

                        auto reader = iw.getIndexReader();
                        auto localDocsVisible = reader->maxDoc();
                        EXPECT_GE(localDocsVisible, globalDocsVisible);
                        while (localDocsVisible > globalDocsVisible) {
                          if (!docsVisible.compare_exchange_weak(globalDocsVisible, localDocsVisible)) {
                            globalDocsVisible = docsVisible.load();
                          }
                        }

                        // LOG_DEBUG("\tvisible docs: {} segs: {}", localDocsVisible, reader->segments().size());

                        /*
                        if (globalDocsVisible >= docsToAdd && globalDocsVisible >= docsAdded) {
                          // we've seen all the docs
                          break;
                        }
                        */
                        if (localDocsVisible >= docsToAdd) {
                          // we've seen all the docs
                          TEST_DEBUG("Reader has all docs visible: localDocsVisible={} docsVisible={}", localDocsVisible, docsVisible.load());
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

    // whait for all threads to finish
    for (auto& t : threads) {
      t.join();
    }

    // wait for all IW activity to finish.
    iw.updateGraph.wait_for_all();

    // If request threads are failing to stop, but this block of code above tasks.wait() and uncomment the sleep
    // std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    while (docsRequested.load() < docsToAdd || docsVisible.load() < docsToAdd) {
      auto reader = iw.getIndexReader();
      LOG_INFO("### Main Thread docsRequested: {}, docsAdded: {}, docsVisible: {}, commitsRequested: {}, commits: {}",
              docsRequested.load(), docsAdded.load(), reader->maxDoc(), commitsRequested.load(), commits.load());


      if (docsRequested.load() >= docsToAdd && reader->maxDoc() < docsToAdd) {
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
        if (reader->maxDoc() == docsToAdd) {
          LOG_ERROR("FINAL COMMIT MADE DOCS VISIBLE! Test Bug or IW bug?");
          FAIL();
        } else {
          break;
        }
      }

      break; // don't loop anymore - nothing should be
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

  // Now index doc2 again with overwrite
  Doc doc2Overwrite = flatdoc("id", "doc2", "text_w", "hello version world again");
  auto result4 = helper.index(doc2Overwrite, UpdateMessage::COMMIT, true);
  req = LocalReq::create(helper.getSearchEngine());
  docs = req->collection("main")
                 .allQuery()
                 .fields({"id", "text_w", "_version_"})
                 .limit(-1)
                 .execute()
                 .getDocs();


  req->done();
  ASSERT_EQ(3, docs.size());

  /* TODO: not ready yet
  expectedDoc2 = flatdoc("id", "doc2", "_version_", (int64_t)(result4.updateVersion));
  foundDoc2 = containsDoc(docs, expectedDoc2);
  EXPECT_TRUE(foundDoc2);
  */
}

// Test deletion functionality - verify delete infrastructure works  
TEST_F(IndexWriterTest, deletionInfrastructure) {
  using namespace solux::test;
  
  CollectionHelper helper("main");
  helper.clear();

  // Add 3 documents with versions (overwrite=true adds _version_ field)
  Doc doc1 = flatdoc("id", "doc1", "text_w", "hello world");
  helper.index(doc1, UpdateMessage::COMMIT, true);

  // for now, put the doc to be deleted in a seg with another doc since we
  // haven't implemented the logic to drop an entire segment!
  Doc doc2 = flatdoc("id", "doc2", "text_w", "goodbye world");
  helper.index(doc2, UpdateMessage::NO_COMMIT, true);

  Doc doc3 = flatdoc("id", "doc3", "text_w", "test document");
  auto result3 = helper.index(doc3, UpdateMessage::COMMIT, true);

  
  // Delete doc2
  auto deleteResult = helper.deleteById("doc2", UpdateMessage::COMMIT);
  EXPECT_TRUE(deleteResult.success);
  EXPECT_GT(deleteResult.updateVersion, result3.updateVersion);
  
  // Verify that the delete was applied at the index level
  auto indexWriter = helper.getIndexWriter();
  // Get a fresh IndexReader after the delete commit
  auto indexReader = indexWriter->getIndexReader(0);  // Force fresh reader

  // Check that at least one segment has deletes applied
  bool foundDeletes = false;
  int32_t totalDeletesFound = 0;
  int32_t totalLiveDocs = 0;
  
  for (const auto& segment : indexReader->segments()) {
    const auto& segInfo = segment.segInfo;
    totalDeletesFound += segment.numDeletes();
    totalLiveDocs += segment.numLive();

    if (segInfo.live_gen > 0) {
      foundDeletes = true;

      // Verify delete metadata is consistent
      EXPECT_GT(segInfo.live_gen, 0);
      EXPECT_NE(segment.liveDocs(), nullptr);
      EXPECT_EQ(segment.numLive(), segInfo.live_docs);
      
      // Verify delete bitmap functionality
      int32_t numDocs = segment.postingsReader().numDocs();
      int32_t deletedCount = 0;
      
      auto* liveDocs = segment.liveDocs();
      ASSERT_NE(liveDocs, nullptr);
      const auto& bitset = liveDocs->bitset();
      
      for (int32_t docId = 0; docId < numDocs; docId++) {
        if (!bitset.get(docId)) {  // if not live, then deleted
          deletedCount++;
        }
      }
      
      EXPECT_EQ(deletedCount, segInfo.max_doc - segInfo.live_docs);
      
      LOG_TRACE("Segment {} has {} live docs (generation {}), verified {} deleted docs of {} total", 
                segInfo.seg_id, segInfo.live_docs, segInfo.live_gen, 
                deletedCount, numDocs);
    } else {
      // Segments without deletes should have consistent state
      EXPECT_EQ(segInfo.live_gen, 0);
      EXPECT_EQ(segment.liveDocs(), nullptr);
      EXPECT_EQ(segment.numDeletes(), 0);
    }
  }
  
  // Verify we found the expected delete
  EXPECT_TRUE(foundDeletes);
  EXPECT_EQ(totalDeletesFound, 1);
  EXPECT_EQ(totalLiveDocs, 2);  // Should have 2 live docs (doc1 and doc3)
  
  // Delete infrastructure verification completed
  
  LOG_TRACE("Delete infrastructure verification completed: {} segments checked, {} total deletes found", 
            indexReader->segments().size(), totalDeletesFound);

  // Now test deleting the same document again - should not create a new live_gen
  uint64_t originalLiveGen = 0;
  for (const auto& segment : indexReader->segments()) {
    const auto& segInfo = segment.segInfo;
    if (segment.numDeletes() > 0) {
      originalLiveGen = segInfo.live_gen;
      break;
    }
  }
  
  EXPECT_GT(originalLiveGen, 0);  // Should have found a segment with deletes
  
  // Delete the same document again
  auto deleteResult2 = helper.deleteById("doc2", UpdateMessage::COMMIT);
  EXPECT_TRUE(deleteResult2.success);
  
  // Get a fresh IndexReader after the second delete
  auto indexReader2 = indexWriter->getIndexReader();
  
  // Verify that live_gen did not increment (no new deletes should be applied)
  bool foundDeletedSegment = false;
  for (const auto& segment : indexReader2->segments()) {
    const auto& segInfo = segment.segInfo;
    if (segment.numDeletes() > 0) {
      foundDeletedSegment = true;
      EXPECT_EQ(segInfo.live_gen, originalLiveGen);
      EXPECT_EQ(segInfo.live_docs, 1);
    }
  }
  EXPECT_TRUE(foundDeletedSegment);

  // Test deleting doc3 to verify entire segment deletion
  // First, let's record the current number of segments
  auto initialSegmentCount = indexReader2->segments().size();
  
  // Delete doc3 (which should be in its own segment)
  auto deleteResult3 = helper.deleteById("doc3", UpdateMessage::COMMIT);
  EXPECT_TRUE(deleteResult3.success);
  EXPECT_GT(deleteResult3.updateVersion, deleteResult2.updateVersion);
  
  // Get a fresh IndexReader after deleting doc3
  auto indexReader3 = indexWriter->getIndexReader(0);  // Force fresh reader
  
  // Verify that the segment containing doc3 has been completely removed
  auto finalSegmentCount = indexReader3->segments().size();
  EXPECT_LT(finalSegmentCount, initialSegmentCount);
  
  // Verify we now have only 1 live document (doc1)
  EXPECT_EQ(indexReader3->maxDoc(), 1);
  
  // Record the segment ID that should have been deleted (doc3's segment)
  // We need to capture this before adding another document that helps triggers the deletion
  // The deletion happens asynchronously.
  uint64_t deletedSegmentId = 0;
  for (const auto& segment : indexReader2->segments()) {
    bool foundInFinalReader = false;
    for (const auto& finalSegment : indexReader3->segments()) {
      if (finalSegment.segInfo.seg_id == segment.segInfo.seg_id) {
        foundInFinalReader = true;
        break;
      }
    }
    if (!foundInFinalReader) {
      deletedSegmentId = segment.segInfo.seg_id;
      break;
    }
  }
  EXPECT_GT(deletedSegmentId, 0); // Should have found a deleted segment
  
  // Add another document to force another commit, which should wait long enough for the segment to be deleted.
  Doc doc4 = flatdoc("id", "doc4", "text_w", "final document");
  helper.index(doc4, UpdateMessage::COMMIT, true);
  
  // Verify that the index files for the deleted segment have actually been removed
  // The deletePrefix method should have removed all files starting with the segment prefix
  std::string deletedPrefix = Postings::getIndexFileNamePrefix(deletedSegmentId);

  // Check that no files exist with the deleted segment prefix
  // Use the Directory's listFiles method to get all files
  auto& dir = helper.getIndexWriter()->dir;
  std::vector<std::string> allFiles;
  dir.listFiles(allFiles);
  
  // Verify that no files exist with the deleted segment's prefix
  for (const auto& filename : allFiles) {
    EXPECT_FALSE(filename.starts_with(deletedPrefix)) 
      << "File " << filename << " should have been deleted by deletePrefix() but still exists";
  }
  
  // Verify the new document is searchable and we now have 2 documents
  auto* req2 = LocalReq::create(helper.getSearchEngine());
  auto finalDocs2 = req2->collection("main")
                      .allQuery()
                      .fields({"id"})
                      .limit(-1)
                      .execute()
                      .getDocs();
  req2->done();
  
  EXPECT_EQ(2, finalDocs2.size()); // doc1 and doc4
}

// test that components in the IndexReader factory methods handle missing files correctly
TEST_F(IndexWriterTest, testMissingFiles) {
  // Create a separate RAMDir for testing the factory methods
  RAMDir testDir;

  // Test missingFileOK = false (should throw on missing files)
  EXPECT_THROW({
      auto reader1 = PostingsReader::create(testDir, 999, false);  // non-existent segment
  }, std::filesystem::filesystem_error);

  // Test missingFileOK = true (should return nullptr on missing files)
  auto reader2 = PostingsReader::create(testDir, 999, true);
  EXPECT_EQ(reader2, nullptr);

  // Test missingFileOK = false (should throw on missing files)
  EXPECT_THROW({
      auto liveDocs1 = LiveDocs::create(testDir, 999, 1, 10, false);  // non-existent segment
  }, std::filesystem::filesystem_error);

  // Test missingFileOK = true (should return nullptr on missing files)
  auto liveDocs2 = LiveDocs::create(testDir, 999, 1, 10, true);
  EXPECT_EQ(liveDocs2, nullptr);
}

// OK CLAUDE, create a random test here to test multithreaded updates and deletes in the IndexWriter and
// reopen/retry logic in the IndexReader.  We will have N threads and M documents that are being modified.
// Eachd document will be assigned to a different thread, conversely eash thread will have it's own set
// of unique documents to modify.  This eliminates any races between threads modifying the same document, and
// each thread can make a modification and then reopen the IndexReader to see the expected changes.
// For documents that should exist, request the _version_ field to verify the doc matches the latest add version.
// Mix in overwrites and deletes randomly, keeping track of the expected state for each thread
// and then verifying it.  This should end up testing much of the logic around segment merging, removal of empty
// segments, IndexReader retry logic, etc.
// You MUST use CollectionHelper to do the indexing and deletions.
// IMPORTANT: Make this test as short as possible.
// IMPORTANT: you should not need any synchronization primitives in the test itself, since each thread will be working
//            with it's own set of documents.
//
TEST_F(IndexWriterTest, testMultithreadedUpdates) {
  using namespace solux::test;
  return; // TODO: test not ready yet. merging and searching deletes not done.
  
  CollectionHelper helper("main");
  helper.clear();
  
  auto indexWriter = helper.getIndexWriter();
  indexWriter->mergePolicy->setMergeFactor(3);

  auto numThreads = 1;
  auto opsPerThread = 100;  // total operations per thread
  auto docsPerThread = 4;   // number of unique documents per thread

  // Track expected versions per thread
  std::vector<std::map<std::string, int64_t>> threadExpectedVersions(numThreads);
  std::vector<std::set<std::string>> threadDeletedDocs(numThreads);
  std::vector<std::thread> threads;

  auto seed = rng();
  for (int tid = 0; tid < numThreads; tid++) {
    threads.emplace_back([&, tid]() {
      Rng r(seed + tid);
      std::vector<int64_t> docVersions(docsPerThread, 0);

      for (int op = 0; op < opsPerThread; op++) {
        // Use numeric IDs: thread 0 uses 1000-1007, thread 1 uses 2000-2007, etc
        // One thread never modifies another threads documents.
        int localDoc = r.rint(docsPerThread);
        std::string docId = std::to_string(localDoc + tid * 1000);
        int64_t lastUpdateVersion = 0;

        int operation = r.rint(3);  // 0 == update, 1 == delete, 2 = read
        
        if (operation == 0) { // Index
          Doc doc = flatdoc("id", docId);
          auto result = helper.index(doc, UpdateMessage::COMMIT, true);
          ASSERT_TRUE(result.success) << "Thread " << tid << " failed to index doc " << docId;
          // TODO: expose and get SoluxError for actual error message / stack trace.
          docVersions[localDoc] = result.updateVersion;

        } else if (operation == 1) { // Delete
          auto result = helper.deleteById(docId, UpdateMessage::COMMIT);
          ASSERT_TRUE(result.success) << "Thread " << tid << " failed to delete doc " << docId;
          docVersions[localDoc] = -1; // Mark as deleted
        } else { // Read
          indexWriter->getIndexReader();
          // TODO: store, expose, and test the update verision in the IndexReader
          
          auto* req = LocalReq::create(helper.getSearchEngine());
          auto docs = req->collection("main")
                        .matchQuery("id", docId)
                        .fields({"id", "_version_"})
                        .execute()
                        .getDocs();
          req->done();
          
          if (docVersions[localDoc] <= 0) {
            EXPECT_TRUE(docs.empty()) << "Thread " << tid << " found deleted doc " << docId;
          } else if (docVersions[localDoc] > 0) {
            EXPECT_EQ(docs.size(), 1) << "Thread " << tid << " doc " << docId << " not found";
            if (!docs.empty()) {
              int64_t foundVersion = -1;
              for (const auto& nv : docs[0]) {
                if (nv.name == "_version_") {
                  foundVersion = std::get<int64_t>(nv.val);
                  break;
                }
              }
              EXPECT_EQ(foundVersion, docVersions[localDoc])
                << "Thread " << tid << " doc " << docId << " version mismatch"
                << " expected: " << docVersions[localDoc] << " found: " << foundVersion;
            }
          }

        } // end Read
      } // end for opsPerThread
    });
  }
  
  // Wait for all threads
  for (auto& thread : threads) {
    thread.join();
  }
  
  // Final commit to hopefully make sure all merges are done.
  helper.commit();
  indexWriter->getIndexReader(0);
  indexWriter->updateGraph.wait_for_all();

  // TODO: fixme : need to restore mergeFactor?  causes FacetBM to fail if it comes after?
  indexWriter->mergePolicy->setMergeFactor(10);
}

// Test segment merging with deleted documents
TEST_F(IndexWriterTest, segmentMergerWithDeletes) {
  using namespace solux::test;
  
  CollectionHelper helper("main");
  helper.clear();
  
  auto indexWriter = helper.getIndexWriter();
  indexWriter->mergePolicy->setMergeFactor(3);
  
  // Create first segment with doc1
  Doc doc1 = flatdoc("id", "doc1", "text_w", "hello world");
  helper.index(doc1, UpdateMessage::COMMIT, true);
  
  // Create second segment with doc2 and doc3 together (tests reordering when doc2 is deleted)
  Doc doc2 = flatdoc("id", "doc2", "text_w", "goodbye world");
  Doc doc3 = flatdoc("id", "doc3", "text_w", "test document");
  helper.index(doc2, UpdateMessage::NO_COMMIT, true);
  helper.index(doc3, UpdateMessage::COMMIT, true);
  
  // Verify we have 2 segments
  auto reader1 = indexWriter->getIndexReader();
  EXPECT_EQ(2, reader1->segments().size());
  EXPECT_EQ(3, reader1->maxDoc());
  
  // Delete doc2 (should be in the second segment with doc3)
  helper.deleteById("doc2", UpdateMessage::COMMIT);
  
  // Verify the delete was applied
  auto reader2 = indexWriter->getIndexReader();
  bool foundDeletes = false;
  for (const auto& segment : reader2->segments()) {
    LOG_INFO("Segment {}: maxDoc={}, liveDocs={}, numDeletes={}, numLive={}", 
             segment.segInfo.seg_id, segment.segInfo.max_doc, 
             segment.segInfo.live_docs, segment.numDeletes(), segment.numLive());
    if (segment.numDeletes() > 0) {
      foundDeletes = true;
      EXPECT_EQ(1, segment.numDeletes());
      EXPECT_EQ(1, segment.numLive());
    }
  }
  EXPECT_TRUE(foundDeletes);
  
  // Add third segment to trigger merge (3 segments total, mergeFactor=3)
  Doc doc4 = flatdoc("id", "doc4", "text_w", "trigger merge");
  helper.index(doc4, UpdateMessage::COMMIT, true);
  
  // Wait for merge to complete
  indexWriter->updateGraph.wait_for_all();
  
  // Get fresh reader
  auto reader3 = indexWriter->getIndexReader();
  
  LOG_INFO("After merge: {} segments, {} total docs", reader3->segments().size(), reader3->maxDoc());
  for (const auto& segment : reader3->segments()) {
    LOG_INFO("Merged segment {}: maxDoc={}, liveDocs={}, numDeletes={}, numLive={}", 
             segment.segInfo.seg_id, segment.segInfo.max_doc, 
             segment.segInfo.live_docs, segment.numDeletes(), segment.numLive());
  }
  
  // Should have fewer segments now due to merge (likely 1 merged segment)
  EXPECT_LE(reader3->segments().size(), 2);
  
  // Should have 3 live documents (doc1, doc3, doc4) - doc2 was deleted and should be excluded from merge
  EXPECT_EQ(3, reader3->maxDoc());
  
  // Verify we can still search and find the expected documents
  auto* req = LocalReq::create(helper.getSearchEngine());
  auto docs = req->collection("main")
                 .allQuery()
                 .fields({"id"})
                 .limit(-1)
                 .execute()
                 .getDocs();
  req->done();
  
  EXPECT_EQ(3, docs.size());
  
  // Verify doc2 is not in results
  bool foundDoc1 = false, foundDoc3 = false, foundDoc4 = false, foundDoc2 = false;
  for (const auto& doc : docs) {
    for (const auto& nv : doc) {
      if (nv.name == "id") {
        std::string id = std::get<std::string>(nv.val);
        if (id == "doc1") foundDoc1 = true;
        if (id == "doc2") foundDoc2 = true;
        if (id == "doc3") foundDoc3 = true;
        if (id == "doc4") foundDoc4 = true;
      }
    }
  }
  
  EXPECT_TRUE(foundDoc1);
  EXPECT_FALSE(foundDoc2); // This should be deleted
  EXPECT_TRUE(foundDoc3);
  EXPECT_TRUE(foundDoc4);
}



// Test that tests remapping of docs and positions after segment merging and deletes.
TEST_F(IndexWriterTest, DISABLED_segmentMergerPositions) {
  using namespace solux::test;

  int docsPerSeg = 10;
  int numSegs = 5;

  CollectionHelper helper("main");
  helper.clear();

  auto indexWriter = helper.getIndexWriter();
  indexWriter->mergePolicy->setMergeFactor(numSegs);

  // Track which documents we're deleting for verification later
  std::set<std::string> deletedIds;

  std::set<std::string> deletes;

  // create 3 segments and delete a random document before committing.
  for (int seg = 0; seg < numSegs; seg++) {
    for (int doc = 0; doc < docsPerSeg; doc++) {
      int id = seg * 100 + doc; // Unique ID for each document
      std::string idStr = "doc" + std::to_string(id);
      // Use same text content to avoid unique term issues during merging
      Doc d = flatdoc("id", id, "text_w", "hello world " + idStr);
      helper.index(d, UpdateMessage::NO_COMMIT, true);
    }
    // now delete some random document in this segment
    // don't delete all of them since we want enough segments to merge together.
    auto nDeletes = rng.rint(1,docsPerSeg-1);
    for (int del = 0; del < nDeletes; del++) {
      int deleteDocId = seg * 100 + (rng.rint(docsPerSeg)); // Delete random doc in each segment
      std::string deleteIdStr = "doc" + std::to_string(deleteDocId);
      deletedIds.insert(deleteIdStr);
      helper.deleteById(deleteIdStr, UpdateMessage::NO_COMMIT);
    }
    // now finally commit
    helper.commit();
  }

  // wait for all merges to complete
  indexWriter->updateGraph.wait_for_all();

  // verify we have a single segment
  auto reader = indexWriter->getIndexReader();
  EXPECT_EQ(1, reader->segments().size()) << "Should have 1 segment after merging";

  auto numDocs = numSegs * docsPerSeg - deletedIds.size();

  // verify deletions have been squeezed out
  EXPECT_EQ(numDocs, reader->maxDoc());

  // Comprehensive verification of document mapping and search functionality
  for (int seg = 0; seg < numSegs; seg++) {
    for (int doc = 0; doc < docsPerSeg; doc++) {
      int id = seg * 100 + doc;
      std::string idStr = "doc" + std::to_string(id);
      std::string termStr = "term" + std::to_string(doc);
      
      // Test 1: Verify term/match queries on the "id" field retrieve the correct "id"
      auto* req = LocalReq::create(helper.getSearchEngine());
      auto docs = req->collection("main")
                     .matchQuery("id", idStr)
                     .fields({"id"})
                     .execute()
                     .getDocs();
      req->done();

      bool wasDeleted = deletedIds.find(idStr) != deletedIds.end();
      if (wasDeleted) {
        ASSERT_TRUE(docs.empty());
      } else {
        ASSERT_EQ(docs.size(), 1);
        bool foundId = false;
        for (const auto& nv : docs[0]) {
          if (nv.name == "id" && std::get<std::string>(nv.val) == idStr) {
            foundId = true;
          }
        }
        EXPECT_TRUE(foundId);
      }

      // Test 2: Verify term/match queries on the "text_w" field for "world" retrieve documents with valid IDs
      req = LocalReq::create(helper.getSearchEngine());
      docs = req->collection("main")
                .matchQuery("text_w", "world")
                .fields({"id", })
                .execute()
                .getDocs();
      req->done();

      // should be all docs
      EXPECT_EQ(numDocs, docs.size());
      
      // TODO: need to do phrase query.

    }
  }
}
